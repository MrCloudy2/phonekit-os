// Multitouch tracking, rotation, latency stats, and synthetic touch injection.
//
// Coordinates: the touchscreen reports panel coordinates scaled to its own
// range; we convert them to panel pixels, then to *logical* pixels for the
// current rotation (what UI code sees). rotation = degrees the UI is turned
// clockwise relative to the panel's native portrait orientation.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static struct fb *fb;

/* ------------------------------------------------------------ transforms */

void touch_set_rotation(int deg) { if (fb) fb_set_rotation(fb, deg); }
int touch_rotation(void) { return fb ? fb->rot : 0; }

void panel_to_logical(int px, int py, int *lx, int *ly) {
    int W = fb->pw, H = fb->ph;
    switch (fb->rot) {
    case 90: *lx = py; *ly = W - 1 - px; break;
    case 180: *lx = W - 1 - px; *ly = H - 1 - py; break;
    case 270: *lx = H - 1 - py; *ly = px; break;
    default: *lx = px; *ly = py;
    }
}

void logical_to_panel(int lx, int ly, int *px, int *py) {
    int W = fb->pw, H = fb->ph;
    switch (fb->rot) {
    case 90: *px = W - 1 - ly; *py = lx; break;
    case 180: *px = W - 1 - lx; *py = H - 1 - ly; break;
    case 270: *px = ly; *py = H - 1 - lx; break;
    default: *px = lx; *py = ly;
    }
}

/* ------------------------------------------------------------ tracking */

#define SLOTS 10
struct finger {
    int active, x, y;       /* panel pixels */
    int has_last, lastx, lasty;
};
/* Per-device state: two touch devices (e.g. the real panel and the injector) must not
 * share fingers, and the input core drops unchanged positions per device, so a
 * shared table would mix one device's last position into another's contacts. */
struct tdev {
    const struct input *dev;
    struct finger f[SLOTS];
    int cur_slot;
    int hid_prev_down[SLOTS];
    int released; /* a contact lifted since the last SYN_REPORT */
};
static struct tdev tdevs[8];
static struct tdev *last_tdev; /* device of the most recent frame (HID reports) */

static struct tdev *state_of(const struct input *dev) {
    struct tdev *free_slot = NULL;
    for (unsigned i = 0; i < sizeof tdevs / sizeof *tdevs; i++) {
        if (tdevs[i].dev == dev) return &tdevs[i];
        if (!tdevs[i].dev && !free_slot) free_slot = &tdevs[i];
    }
    if (!free_slot) free_slot = &tdevs[0];
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->dev = dev;
    return free_slot;
}
/* long-press in the top-left corner (on-screen menu trigger) */
#define CORNER_PX 220
static long corner_since = -1;
static int last_lx = -1, last_ly = -1, last_px = -1, last_py = -1;
static unsigned long touches_total, frames_flushed;

/* latency: kernel event timestamp (CLOCK_BOOTTIME) -> fb flushed */
static long lat_min_us = -1, lat_max_us, lat_sum_us, lat_n;

static const uint8_t slot_rgb[SLOTS][3] = {
    {80, 220, 255}, {255, 120, 80}, {120, 255, 120}, {255, 220, 60}, {200, 120, 255},
    {255, 90, 200}, {90, 140, 255}, {255, 255, 255}, {160, 255, 220}, {255, 170, 120}};

void touch_init(struct fb *f) { fb = f; }

/* Returns 1 once when a corner long-press of `ms` completes (then re-arms on lift). */
int touch_corner_hold(int ms) {
    if (corner_since >= 0 && now_ms() - corner_since >= ms) { corner_since = -2; return 1; }
    return 0;
}
int touch_corner_pending(void) { return corner_since >= 0; }

void touch_use_boottime(int evfd) {
    int clk = CLOCK_BOOTTIME;
    ioctl(evfd, EVIOCSCLOCKID, &clk); /* timestamps comparable with now_ms() */
}

static void draw_segment(int slot, int x0, int y0, int x1, int y1) {
    panel_to_logical(x0, y0, &x0, &y0); /* fingers are panel px, drawing is logical */
    panel_to_logical(x1, y1, &x1, &y1);
    uint32_t c = fb_color(fb, slot_rgb[slot][0], slot_rgb[slot][1], slot_rgb[slot][2]);
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i += 2) /* 12 px brush, step 2 is plenty */
        fb_fill(fb, x0 + dx * i / steps - 6, y0 + dy * i / steps - 6, 12, 12, c);
}

/* Feed one event from a touch device. Returns 1 if a frame needs flushing now. */
int touch_event(const struct input *dev, const struct input_event *e) {
    struct tdev *t = state_of(dev);
    struct finger *fingers = t->f;
    if (e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT && e->value >= 0 && e->value < SLOTS) t->cur_slot = e->value;
        struct finger *f = &fingers[t->cur_slot];
        if (e->code == ABS_MT_TRACKING_ID) {
            if (e->value < 0) { if (f->active) t->released = 1; f->active = 0; f->has_last = 0; }
            else { f->active = 1; f->has_last = 0; touches_total++; }
        } else if (e->code == ABS_MT_POSITION_X && dev->absmax_x > 0)
            f->x = e->value * (fb->pw - 1) / dev->absmax_x;
        else if (e->code == ABS_MT_POSITION_Y && dev->absmax_y > 0)
            f->y = e->value * (fb->ph - 1) / dev->absmax_y;
        return 0;
    }
    if (e->type != EV_SYN || e->code != SYN_REPORT) return 0;
    last_tdev = t;
    int dirty = 0;
    int nactive = 0, one = -1;
    for (int s = 0; s < SLOTS; s++) if (fingers[s].active) { nactive++; one = s; }
    /* exactly one finger (any slot) resting in the corner starts/keeps the long-press timer */
    int clx = 0, cly = 0;
    if (one >= 0) panel_to_logical(fingers[one].x, fingers[one].y, &clx, &cly);
    if (nactive == 1 && clx < CORNER_PX && cly < CORNER_PX) {
        if (corner_since == -1) corner_since = now_ms(); /* -2 = fired, wait for lift */
    } else corner_since = -1;
    for (int s = 0; s < SLOTS; s++) {
        struct finger *f = &fingers[s];
        if (!f->active) continue;
        if (fb && fb->mem && hm_owner() == OWN_PAGE && ui_page() == UI_PAGE_TEST) { /* paint only on the test page */
            if (f->has_last) draw_segment(s, f->lastx, f->lasty, f->x, f->y);
            else draw_segment(s, f->x, f->y, f->x, f->y);
            dirty = 1;
        }
        f->lastx = f->x; f->lasty = f->y; f->has_last = 1;
        last_px = f->x; last_py = f->y;
        panel_to_logical(f->x, f->y, &last_lx, &last_ly);
    }
    /* A touch frame happened (even if nothing was drawn, e.g. while the host owns the
     * screen in GUD mode): the caller forwards it over HID and flushes if dirty. */
    /* release frames count too: taps and key presses complete on lift */
    int frame = t->released;
    t->released = 0;
    for (int s = 0; s < SLOTS; s++) if (fingers[s].active || t->hid_prev_down[s]) frame = 1;
    return frame || dirty;
}

/* Active contacts of the most recent frame, in panel pixels (for trackpad/keyboard/menu). */
int touch_active_px(int *ids, int *xs, int *ys, int max) {
    int n = 0;
    if (!last_tdev) return 0;
    for (int s = 0; s < SLOTS && n < max; s++)
        if (last_tdev->f[s].active) { ids[n] = s; panel_to_logical(last_tdev->f[s].x, last_tdev->f[s].y, &xs[n], &ys[n]); n++; }
    return n;
}

/* Contacts to report over USB HID for this frame: every active finger, plus fingers
 * that were down in the previous report and are now up (tip switch 0, once). */
int touch_hid_contacts(int *ids, int *xs, int *ys, int *down, int max) {
    int n = 0;
    if (!last_tdev) return 0;
    struct finger *fingers = last_tdev->f;
    int *hid_prev_down = last_tdev->hid_prev_down;
    for (int s = 0; s < SLOTS && n < max; s++) {
        struct finger *f = &fingers[s];
        if (!f->active && !hid_prev_down[s]) continue;
        ids[n] = s;
        xs[n] = fb && fb->pw > 1 ? (int)((long)f->x * 32767 / (fb->pw - 1)) : 0;
        ys[n] = fb && fb->ph > 1 ? (int)((long)f->y * 32767 / (fb->ph - 1)) : 0;
        down[n] = f->active;
        hid_prev_down[s] = f->active;
        n++;
    }
    return n;
}

/* Call right after the flush that showed this SYN_REPORT. */
void touch_flushed(const struct input_event *syn) {
    struct timespec now;
    clock_gettime(CLOCK_BOOTTIME, &now);
    long us = (now.tv_sec - syn->input_event_sec) * 1000000L + (now.tv_nsec / 1000 - syn->input_event_usec);
    if (us < 0 || us > 10000000) return; /* clock mismatch (device without BOOTTIME): ignore */
    frames_flushed++;
    if (lat_min_us < 0 || us < lat_min_us) lat_min_us = us;
    if (us > lat_max_us) lat_max_us = us;
    lat_sum_us += us;
    lat_n++;
}

int touch_stats(char *buf, size_t len) {
    return snprintf(buf, len,
                    "rotation %d\ntouches %lu\nframes %lu\nlast_panel %d %d\nlast_logical %d %d\n"
                    "latency_us min %ld avg %ld max %ld n %ld\n",
                    fb ? fb->rot : 0, touches_total, frames_flushed, last_px, last_py, last_lx, last_ly,
                    lat_min_us, lat_n ? lat_sum_us / lat_n : -1, lat_max_us, lat_n);
}

void touch_stats_reset(void) {
    lat_min_us = -1; lat_max_us = lat_sum_us = lat_n = 0;
    touches_total = frames_flushed = 0;
    last_lx = last_ly = last_px = last_py = -1;
}

/* ------------------------------------------------------------ injection */
// A uinput multitouch device shaped like the real panel, so injected events take
// the full kernel -> evdev -> paneld path. Coordinates given in panel pixels.

static int ui_fd = -1;

static int ui_emit(int type, int code, int value) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    return write(ui_fd, &ev, sizeof ev) == sizeof ev ? 0 : -1;
}

int inject_open(void) {
    if (ui_fd >= 0) return 0;
    ensure_node("misc", "uinput", "/dev/uinput");
    ui_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ui_fd < 0) { logmsg("inject: /dev/uinput: %s", strerror(errno)); return -1; }
    ioctl(ui_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(ui_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ui_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(ui_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    struct uinput_user_dev u = {0};
    snprintf(u.name, sizeof u.name, "phonepanel-inject");
    u.id.bustype = BUS_VIRTUAL;
    int axes[] = {ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y};
    int maxv[] = {SLOTS - 1, 65535, fb->pw - 1, fb->ph - 1};
    for (int i = 0; i < 4; i++) {
        ioctl(ui_fd, UI_SET_ABSBIT, axes[i]);
        u.absmax[axes[i]] = maxv[i];
    }
    if (write(ui_fd, &u, sizeof u) != sizeof u || ioctl(ui_fd, UI_DEV_CREATE) < 0) {
        logmsg("inject: create: %s", strerror(errno));
        close(ui_fd); ui_fd = -1;
        return -1;
    }
    logmsg("inject: virtual touchscreen created");
    return 0;
}

static int tracking_id = 1;

/* One contact moving through n points (panel px), dt_ms between frames. */
int inject_stroke(int slot, const int *xs, const int *ys, int n, int dt_ms) {
    if (inject_open() < 0) return -1;
    ui_emit(EV_ABS, ABS_MT_SLOT, slot);
    ui_emit(EV_ABS, ABS_MT_TRACKING_ID, tracking_id++);
    ui_emit(EV_KEY, BTN_TOUCH, 1);
    for (int i = 0; i < n; i++) {
        ui_emit(EV_ABS, ABS_MT_SLOT, slot);
        ui_emit(EV_ABS, ABS_MT_POSITION_X, xs[i]);
        ui_emit(EV_ABS, ABS_MT_POSITION_Y, ys[i]);
        ui_emit(EV_SYN, SYN_REPORT, 0);
        if (dt_ms) usleep(dt_ms * 1000);
    }
    ui_emit(EV_ABS, ABS_MT_SLOT, slot);
    ui_emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ui_emit(EV_KEY, BTN_TOUCH, 0);
    return ui_emit(EV_SYN, SYN_REPORT, 0);
}

/* k simultaneous contacts, each a short vertical drag, starting at given x positions. */
int inject_multi(int k, int y0, int y1, int steps) {
    if (inject_open() < 0) return -1;
    if (k > SLOTS) k = SLOTS;
    for (int s = 0; s < k; s++) {
        ui_emit(EV_ABS, ABS_MT_SLOT, s);
        ui_emit(EV_ABS, ABS_MT_TRACKING_ID, tracking_id++);
    }
    ui_emit(EV_KEY, BTN_TOUCH, 1);
    for (int i = 0; i <= steps; i++) {
        for (int s = 0; s < k; s++) {
            ui_emit(EV_ABS, ABS_MT_SLOT, s);
            ui_emit(EV_ABS, ABS_MT_POSITION_X, (s + 1) * fb->pw / (k + 1));
            ui_emit(EV_ABS, ABS_MT_POSITION_Y, y0 + (y1 - y0) * i / (steps ? steps : 1));
        }
        ui_emit(EV_SYN, SYN_REPORT, 0);
        usleep(8000);
    }
    for (int s = 0; s < k; s++) {
        ui_emit(EV_ABS, ABS_MT_SLOT, s);
        ui_emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    }
    ui_emit(EV_KEY, BTN_TOUCH, 0);
    return ui_emit(EV_SYN, SYN_REPORT, 0);
}

/* A second uinput device with the phone's hardware keys, so key handling can be
 * tested through the real evdev path. */
static int uk_fd = -1;
int inject_key_open(void) {
    if (uk_fd < 0) {
        uk_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (uk_fd < 0) return -1;
        ioctl(uk_fd, UI_SET_EVBIT, EV_KEY);
        int codes[] = {KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_XCOVER, KEY_BIXBY, KEY_MACRO17};
        for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) ioctl(uk_fd, UI_SET_KEYBIT, codes[i]);
        struct uinput_user_dev u = {0};
        snprintf(u.name, sizeof u.name, "phonepanel-inject-keys");
        u.id.bustype = BUS_VIRTUAL;
        if (write(uk_fd, &u, sizeof u) != sizeof u || ioctl(uk_fd, UI_DEV_CREATE) < 0) {
            close(uk_fd); uk_fd = -1;
            return -1;
        }
        logmsg("inject: virtual keys created");
    }
    return 0;
}

int inject_key(int code, int hold_ms) { /* call in a child after inject_key_open() in the parent */
    if (uk_fd < 0) return -1;
    struct input_event ev = {.type = EV_KEY, .code = code, .value = 1}, syn = {.type = EV_SYN};
    if (write(uk_fd, &ev, sizeof ev) < 0 || write(uk_fd, &syn, sizeof syn) < 0) return -1;
    usleep(hold_ms * 1000);
    ev.value = 0;
    if (write(uk_fd, &ev, sizeof ev) < 0 || write(uk_fd, &syn, sizeof syn) < 0) return -1;
    return 0;
}
