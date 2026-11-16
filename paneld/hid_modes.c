// Mode A input modes: touchscreen (HID digitizer + GUD), trackpad (HID mouse),
// keyboard (on-screen keyboard -> HID boot keyboard, with a trackpad strip), and the
// mode menu. Also decides who owns the screen.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct fb *fb;
static int last_owner = -1;
static int input_mode = IN_TOUCHSCREEN;
static int menu_open, menu_sel;
static int settings_open, set_sel;
static const char *const mode_name[IN_MODES] = {"SCREEN", "MOUSE", "KEYBOARD", "COMBO"};

int hm_input_mode(void) { return input_mode; }
int hm_menu_open(void) { return menu_open; }

int hm_owner(void) {
    if (menu_open || settings_open) return OWN_MENU;
    if (gadget_mode() == 'A' && input_mode != IN_TOUCHSCREEN) return OWN_INPUT;
    if (gadget_mode() == 'A' && gud_active()) return OWN_GUD;
    return OWN_PAGE;
}

/* ---------------------------------------------------------------- HID output */

static int hid_open(int *fd, const char *path) {
    if (*fd < 0) *fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    return *fd;
}

static int mouse_fd = -1, kbd_fd = -1;
static int buttons; /* bit 0 left, 1 right, 2 middle */

static void mouse_report(int dx, int dy, int wheel, int pan) {
    if (hid_open(&mouse_fd, "/dev/hidg1") < 0) return;
    dx = dx > 32767 ? 32767 : dx < -32767 ? -32767 : dx;
    dy = dy > 32767 ? 32767 : dy < -32767 ? -32767 : dy;
    uint8_t r[7] = {(uint8_t)buttons, dx & 0xff, (dx >> 8) & 0xff, dy & 0xff, (dy >> 8) & 0xff,
                    (uint8_t)(int8_t)wheel, (uint8_t)(int8_t)pan};
    if (write(mouse_fd, r, sizeof r) < 0 && errno != EAGAIN) { close(mouse_fd); mouse_fd = -1; }
}

static void click(int bit) { /* press + release */
    buttons |= bit; mouse_report(0, 0, 0, 0);
    usleep(15000);
    buttons &= ~bit; mouse_report(0, 0, 0, 0);
}

static void kbd_report(int mods, int code) {
    if (hid_open(&kbd_fd, "/dev/hidg2") < 0) return;
    uint8_t r[8] = {(uint8_t)mods, 0, (uint8_t)code, 0, 0, 0, 0, 0};
    if (write(kbd_fd, r, sizeof r) < 0 && errno != EAGAIN) { close(kbd_fd); kbd_fd = -1; }
}

static void key_tap(int mods, int code) {
    kbd_report(mods, code);
    usleep(12000);
    kbd_report(0, 0);
}

/* ---------------------------------------------------------------- trackpad */
// Gesture tracking over a region (whole screen in trackpad mode, the top strip in
// keyboard mode). One finger moves; tap = left, 2-finger tap = right, 3-finger tap =
// middle; two fingers drag = scroll. In trackpad mode the bottom strip is L | R buttons.

#define BTN_ZONE_H 300
#define TAP_MS 220
#define TAP_SLOP_PX 30

static struct {
    int y0, y1;              /* region (panel px) */
    int with_buttons;
    int fingers;             /* fingers currently in the pad region */
    int max_fingers;         /* during this gesture */
    long start_ms;
    int moved;               /* total movement, px */
    int lastx, lasty, lastn; /* previous centroid */
    int scroll_acc_x, scroll_acc_y;
    float rem_x, rem_y;
} tp;

static float tp_sens(void) { return store_get_int("tp.sens_pct", 160) / 100.0f; }

static void trackpad_frame(const int *xs, const int *ys, int n) {
    /* split fingers: button zone vs pad */
    int px[10], py[10], np = 0, want_buttons = 0;
    int bz = tp.with_buttons ? fb->h - BTN_ZONE_H : fb->h + 1;
    for (int i = 0; i < n; i++) {
        if (ys[i] < tp.y0 || ys[i] >= tp.y1) continue;
        if (tp.with_buttons && ys[i] >= bz) { want_buttons |= xs[i] < fb->w / 2 ? 1 : 2; continue; }
        px[np] = xs[i]; py[np] = ys[i]; np++;
    }
    if (tp.with_buttons && want_buttons != (buttons & 3)) {
        buttons = (buttons & ~3) | want_buttons;
        mouse_report(0, 0, 0, 0);
    }
    long now = now_ms();
    if (np == 0) {
        if (tp.fingers > 0) { /* gesture ended: tap? */
            if (now - tp.start_ms < TAP_MS && tp.moved < TAP_SLOP_PX)
                click(tp.max_fingers >= 3 ? 4 : tp.max_fingers == 2 ? 2 : 1);
        }
        tp.fingers = tp.max_fingers = 0;
        return;
    }
    int cx = 0, cy = 0;
    for (int i = 0; i < np; i++) { cx += px[i]; cy += py[i]; }
    cx /= np; cy /= np;
    if (tp.fingers == 0) { tp.start_ms = now; tp.moved = 0; tp.max_fingers = 0; tp.scroll_acc_x = tp.scroll_acc_y = 0; }
    if (np > tp.max_fingers) tp.max_fingers = np;
    if (np != tp.lastn || tp.fingers == 0) { tp.lastx = cx; tp.lasty = cy; tp.lastn = np; tp.fingers = np; return; }
    int dx = cx - tp.lastx, dy = cy - tp.lasty;
    tp.lastx = cx; tp.lasty = cy; tp.fingers = np;
    tp.moved += abs(dx) + abs(dy);
    if (np == 1) {
        /* pointer acceleration: faster swipes travel further */
        float v = (float)(abs(dx) + abs(dy));
        float g = tp_sens() * (1.0f + (v > 40 ? 40 : v) / 25.0f);
        tp.rem_x += dx * g; tp.rem_y += dy * g;
        int mx = (int)tp.rem_x, my = (int)tp.rem_y;
        tp.rem_x -= mx; tp.rem_y -= my;
        if (mx || my) mouse_report(mx, my, 0, 0);
    } else if (np == 2) { /* scroll: one wheel step per 40 px, natural direction */
        tp.scroll_acc_y += dy; tp.scroll_acc_x += dx;
        int w = tp.scroll_acc_y / 40, h = tp.scroll_acc_x / 40;
        if (w || h) {
            tp.scroll_acc_y -= w * 40; tp.scroll_acc_x -= h * 40;
            mouse_report(0, 0, w, -h);
        }
    }
}

static void draw_trackpad(void) {
    uint32_t bg = fb_color(fb, 18, 20, 28), fg = fb_color(fb, 120, 130, 155), line = fb_color(fb, 45, 50, 66);
    fb_fill(fb, 0, 0, fb->w, fb->h, bg);
    fb_text(fb, 60, 80, 6, "MOUSE", fb_color(fb, 210, 215, 230), bg);
    fb_text(fb, 60, 160, 3, "1 FINGER MOVE   TAP CLICK   2 FINGERS SCROLL", fg, bg);
    fb_text(fb, 60, 200, 3, "2-FINGER TAP RIGHT   3-FINGER TAP MIDDLE", fg, bg);
    fb_text(fb, 60, 240, 3, "XCOVER: NEXT MODE   HOLD XCOVER: MENU", fg, bg);
    int bz = fb->h - BTN_ZONE_H;
    fb_fill(fb, 0, bz, fb->w, 4, line);
    fb_fill(fb, fb->w / 2 - 2, bz, 4, BTN_ZONE_H, line);
    fb_text(fb, fb->w / 4 - 40, bz + BTN_ZONE_H / 2 - 30, 8, "L", fg, bg);
    fb_text(fb, fb->w * 3 / 4 - 40, bz + BTN_ZONE_H / 2 - 30, 8, "R", fg, bg);
}

/* ---------------------------------------------------------------- keyboard */

#define M_CTRL 0x01
#define M_SHIFT 0x02
#define M_ALT 0x04
#define M_GUI 0x08

struct key { const char *label; int code; int mod; int w; /* width in tenths of a row */ };
/* HID usage codes: a=0x04.., 1=0x1e.., 0=0x27, enter 0x28, esc 0x29, bksp 0x2a, tab 0x2b, space 0x2c */
static const struct key rows[][12] = {
    {{"ESC", 0x29, 0, 10}, {"1", 0x1e, 0, 10}, {"2", 0x1f, 0, 10}, {"3", 0x20, 0, 10}, {"4", 0x21, 0, 10},
     {"5", 0x22, 0, 10}, {"6", 0x23, 0, 10}, {"7", 0x24, 0, 10}, {"8", 0x25, 0, 10}, {"9", 0x26, 0, 10}, {"0", 0x27, 0, 10}, {0}},
    {{"Q", 0x14, 0, 10}, {"W", 0x1a, 0, 10}, {"E", 0x08, 0, 10}, {"R", 0x15, 0, 10}, {"T", 0x17, 0, 10},
     {"Y", 0x1c, 0, 10}, {"U", 0x18, 0, 10}, {"I", 0x0c, 0, 10}, {"O", 0x12, 0, 10}, {"P", 0x13, 0, 10}, {"-", 0x2d, 0, 10}, {0}},
    {{"A", 0x04, 0, 10}, {"S", 0x16, 0, 10}, {"D", 0x07, 0, 10}, {"F", 0x09, 0, 10}, {"G", 0x0a, 0, 10},
     {"H", 0x0b, 0, 10}, {"J", 0x0d, 0, 10}, {"K", 0x0e, 0, 10}, {"L", 0x0f, 0, 10}, {";", 0x33, 0, 10}, {"'", 0x34, 0, 10}, {0}},
    {{"SHF", 0, M_SHIFT, 15}, {"Z", 0x1d, 0, 10}, {"X", 0x1b, 0, 10}, {"C", 0x06, 0, 10}, {"V", 0x19, 0, 10},
     {"B", 0x05, 0, 10}, {"N", 0x11, 0, 10}, {"M", 0x10, 0, 10}, {",", 0x36, 0, 10}, {".", 0x37, 0, 10}, {"DEL", 0x2a, 0, 15}, {0}},
    {{"CTL", 0, M_CTRL, 13}, {"ALT", 0, M_ALT, 13}, {"SUP", 0, M_GUI, 12}, {"/", 0x38, 0, 10},
     {"SPACE", 0x2c, 0, 32}, {"TAB", 0x2b, 0, 15}, {"ENT", 0x28, 0, 15}, {0}},
    {{"LT", 0x50, 0, 22}, {"UP", 0x52, 0, 22}, {"DN", 0x51, 0, 22}, {"RT", 0x4f, 0, 22}, {"=", 0x2e, 0, 22}, {0}},
};
#define NROWS (int)(sizeof rows / sizeof rows[0])
#define KB_TOP (input_mode == IN_COMBO ? fb->h * 38 / 100 : fb->h * 12 / 100)
#define ROW_H ((fb->h - KB_TOP) / NROWS)

static int sticky; /* modifiers armed for the next key */
static int kb_down_row = -1, kb_down_col = -1, kb_down_id = -1;
static long kb_down_ms, kb_repeat_ms;

static int row_units(int r) { int u = 0; for (int c = 0; rows[r][c].label; c++) u += rows[r][c].w; return u; }

static int key_at(int x, int y, int *col) {
    if (y < KB_TOP) return -1;
    int r = (y - KB_TOP) / ROW_H;
    if (r >= NROWS) r = NROWS - 1;
    int total = row_units(r), acc = 0;
    for (int c = 0; rows[r][c].label; c++) {
        int x1 = (acc + rows[r][c].w) * fb->w / total;
        if (x < x1) { *col = c; return r; }
        acc += rows[r][c].w;
    }
    return -1;
}

static void draw_key(int r, int c, int pressed) {
    int total = row_units(r), acc = 0;
    for (int i = 0; i < c; i++) acc += rows[r][i].w;
    int x0 = acc * fb->w / total, x1 = (acc + rows[r][c].w) * fb->w / total;
    int y0 = KB_TOP + r * ROW_H;
    const struct key *k = &rows[r][c];
    int armed = k->mod && (sticky & k->mod);
    uint32_t bg = pressed ? fb_color(fb, 70, 120, 200) : armed ? fb_color(fb, 50, 90, 150) : fb_color(fb, 38, 42, 56);
    fb_fill(fb, x0, y0, x1 - x0, ROW_H, fb_color(fb, 14, 16, 22));
    fb_fill(fb, x0 + 4, y0 + 4, x1 - x0 - 8, ROW_H - 8, bg);
    int len = (int)strlen(k->label), sc = len > 3 ? 4 : len > 1 ? 5 : 7;
    fb_text(fb, (x0 + x1) / 2 - len * 6 * sc / 2, y0 + ROW_H / 2 - 4 * sc, sc, k->label, fb_color(fb, 235, 238, 245), bg);
}

static void draw_keyboard(void) {
    uint32_t bg = fb_color(fb, 18, 20, 28);
    fb_fill(fb, 0, 0, fb->w, KB_TOP, bg);
    fb_text(fb, 40, 40, 4, input_mode == IN_COMBO ? "COMBO - TRACKPAD ABOVE KEYS" : "KEYBOARD", fb_color(fb, 120, 130, 155), bg);
    fb_fill(fb, 0, KB_TOP - 4, fb->w, 4, fb_color(fb, 45, 50, 66));
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; rows[r][c].label; c++) draw_key(r, c, 0);
}

static void emit_key(const struct key *k) {
    if (k->mod) { sticky ^= k->mod; return; }
    key_tap(sticky, k->code);
    int had = sticky;
    sticky = 0;
    if (had) /* redraw the modifier keys as released */
        for (int r = 0; r < NROWS; r++)
            for (int c = 0; rows[r][c].label; c++) if (rows[r][c].mod) draw_key(r, c, 0);
}

static void keyboard_frame(const int *ids, const int *xs, const int *ys, int n) {
    /* first finger that went down on a key owns it until it lifts */
    int still = 0;
    for (int i = 0; i < n; i++) if (ids[i] == kb_down_id) {
        still = 1;
        int c, r = key_at(xs[i], ys[i], &c);
        if (r != kb_down_row || c != kb_down_col) { /* slid off: cancel */
            if (kb_down_row >= 0) draw_key(kb_down_row, kb_down_col, 0);
            kb_down_row = kb_down_col = -1;
        }
    }
    if (kb_down_id >= 0 && !still) { /* released */
        if (kb_down_row >= 0) {
            const struct key *k = &rows[kb_down_row][kb_down_col];
            if (now_ms() - kb_down_ms < 500 || k->code != 0x2a) emit_key(k); /* held DEL already repeated */
            draw_key(kb_down_row, kb_down_col, 0);
        }
        kb_down_id = kb_down_row = kb_down_col = -1;
    }
    if (kb_down_id < 0)
        for (int i = 0; i < n; i++) {
            int c, r = key_at(xs[i], ys[i], &c);
            if (r < 0) continue;
            kb_down_id = ids[i]; kb_down_row = r; kb_down_col = c;
            kb_down_ms = kb_repeat_ms = now_ms();
            draw_key(r, c, 1);
            break;
        }
    /* fingers above the keys drive the trackpad strip (combo mode only) */
    if (input_mode != IN_COMBO) return;
    int tx[10], ty[10], m = 0;
    for (int i = 0; i < n; i++) if (ys[i] < KB_TOP && ids[i] != kb_down_id) { tx[m] = xs[i]; ty[m] = ys[i]; m++; }
    trackpad_frame(tx, ty, m);
}

/* ---------------------------------------------------------------- menu */

/* top-level phone modes, in the order XCover-short cycles through them */
static const char *const menu_items[] = {"PHONEBOX", "SCREEN", "MOUSE", "KEYBOARD", "COMBO", "WEBCAM (LATER)", "SETTINGS", "CLOSE"};
#define MENU_N 8
enum { MI_PANEL, MI_SCREEN, MI_MOUSE, MI_KEYBOARD, MI_COMBO, MI_WEBCAM, MI_SETTINGS, MI_CLOSE };

/* ---------------------------------------------------------------- settings */
#define SET_N 8
static const char *const set_names[SET_N] = {"ROTATION", "BRIGHTNESS", "DIM AFTER", "SCREEN OFF", "CHARGE LIMIT", "USB NETWORK", "WHEN PC IS GONE", "BACK"};
static const char *const standby_vals[] = {"hold", "ambient", "sleep"};
static const int rot_vals[] = {-1, 0, 90, 180, 270};           /* -1 = auto */
static const int dim_vals[] = {30, 60, 120, 300, 0};           /* 0 = never */
static const int off_vals[] = {60, 300, 900, 1800, 0};
static const int chg_lo[] = {40, 50, 70, 0}, chg_hi[] = {60, 80, 90, 100}; /* last = no limit */

static int idx_of(const int *v, int n, int x) { for (int i = 0; i < n; i++) if (v[i] == x) return i; return 0; }

static void set_value(int i, char *buf, size_t len) {
    switch (i) {
    case 0: { int r = store_get_int("rotation", -1);
              if (r < 0) snprintf(buf, len, "AUTO (NOW %d)", fb->rot); else snprintf(buf, len, "%d", r); break; }
    case 1: snprintf(buf, len, "%d", pcfg.bright); break;
    case 2: pcfg.dim_after_s ? snprintf(buf, len, "%d S", pcfg.dim_after_s) : snprintf(buf, len, "NEVER"); break;
    case 3: pcfg.off_after_s ? snprintf(buf, len, "%d MIN", pcfg.off_after_s / 60) : snprintf(buf, len, "NEVER"); break;
    case 4: pcfg.limiter ? snprintf(buf, len, "%d-%d %%", pcfg.charge_lo, pcfg.charge_hi) : snprintf(buf, len, "OFF"); break;
    case 5: snprintf(buf, len, "%s (REBOOT)", store_get("usb_net", "rndis")); break;
    case 6: { const char *v = store_get("standby", "hold");
              snprintf(buf, len, "%s", !strcmp(v, "ambient") ? "AMBIENT CLOCK" : !strcmp(v, "sleep") ? "SCREEN OFF" : "KEEP LAST PAGE"); break; }
    default: buf[0] = 0;
    }
}

#define SET_Y0 260
#define SET_ITEM_H ((fb->h - SET_Y0 - 80) / SET_N)

static void draw_settings(void) {
    uint32_t bg = fb_color(fb, 10, 12, 18), fg = fb_color(fb, 235, 238, 245), dim = fb_color(fb, 150, 190, 255);
    fb_fill(fb, 0, 0, fb->w, fb->h, bg);
    fb_text(fb, 60, 100, 7, "SETTINGS", fg, bg);
    int sc = fb->w > fb->h ? 4 : 5;
    for (int i = 0; i < SET_N; i++) {
        int y = SET_Y0 + i * SET_ITEM_H;
        uint32_t b = i == set_sel ? fb_color(fb, 50, 95, 170) : fb_color(fb, 32, 36, 48);
        fb_fill(fb, 40, y + 8, fb->w - 80, SET_ITEM_H - 16, b);
        fb_text(fb, 80, y + SET_ITEM_H / 2 - 4 * sc - 18, sc, set_names[i], fg, b);
        char v[48];
        set_value(i, v, sizeof v);
        fb_text(fb, 80, y + SET_ITEM_H / 2 + 6, sc - 1, v, dim, b);
    }
}

static void save_int(const char *k, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); store_set(k, b); }

static void redraw(void);

/* Steps the value of setting i (dir +1/-1). */
static void set_step(int i, int dir) {
    switch (i) {
    case 0: { int k = idx_of(rot_vals, 5, store_get_int("rotation", -1)); k = (k + dir + 5) % 5;
              save_int("rotation", rot_vals[k]);
              hm_set_rotation(rot_vals[k] < 0 ? fb->rot : rot_vals[k]); return; /* redraws */ }
    case 1: power_bright_step(dir); break;
    case 2: { int k = (idx_of(dim_vals, 5, pcfg.dim_after_s) + dir + 5) % 5; pcfg.dim_after_s = dim_vals[k];
              save_int("power.dim_after_s", pcfg.dim_after_s); break; }
    case 3: { int k = (idx_of(off_vals, 5, pcfg.off_after_s) + dir + 5) % 5; pcfg.off_after_s = off_vals[k];
              save_int("power.off_after_s", pcfg.off_after_s); break; }
    case 4: { int k = pcfg.limiter ? idx_of(chg_lo, 3, pcfg.charge_lo) : 3; k = (k + dir + 4) % 4;
              pcfg.limiter = k < 3; pcfg.charge_lo = chg_lo[k]; pcfg.charge_hi = chg_hi[k];
              save_int("power.limiter", pcfg.limiter); save_int("power.charge_lo", pcfg.charge_lo);
              save_int("power.charge_hi", pcfg.charge_hi); break; }
    case 5: store_set("usb_net", strcmp(store_get("usb_net", "rndis"), "rndis") ? "rndis" : "ncm"); break;
    case 6: { const char *v = store_get("standby", "hold");
              int k = !strcmp(v, "ambient") ? 1 : !strcmp(v, "sleep") ? 2 : 0;
              store_set("standby", standby_vals[(k + dir + 3) % 3]); break; }
    case 7: settings_open = 0; redraw(); return;
    }
    draw_settings();
    fb_present(fb);
}
#define MENU_Y0 300
#define MENU_ITEM_H ((fb->h - MENU_Y0 - 100) / MENU_N)

static void draw_menu(void) {
    uint32_t bg = fb_color(fb, 10, 12, 18), fg = fb_color(fb, 235, 238, 245);
    fb_fill(fb, 0, 0, fb->w, fb->h, bg);
    fb_text(fb, 60, 120, 8, "MODE", fg, bg);
    char cur[48];
    snprintf(cur, sizeof cur, "NOW: %s   (USB %c)", gadget_mode() == 'A' ? mode_name[input_mode] : "PHONEBOX", gadget_mode());
    fb_text(fb, 60, 220, 3, cur, fb_color(fb, 120, 130, 155), bg);
    for (int i = 0; i < MENU_N; i++) {
        int y = MENU_Y0 + i * MENU_ITEM_H;
        uint32_t b = i == menu_sel ? fb_color(fb, 50, 95, 170) : fb_color(fb, 32, 36, 48);
        fb_fill(fb, 40, y + 10, fb->w - 80, MENU_ITEM_H - 20, b);
        fb_text(fb, 90, y + MENU_ITEM_H / 2 - 28, 7, menu_items[i], fg, b);
    }
}

static void redraw(void);

static int current_item(void) { return gadget_mode() == 'A' ? MI_SCREEN + input_mode : MI_PANEL; }

static void menu_choose(int i) {
    menu_open = 0;
    if (i >= MI_SCREEN && i <= MI_COMBO) {
        input_mode = i - MI_SCREEN;
        char v[4];
        snprintf(v, sizeof v, "%d", input_mode);
        store_set("input_mode", v);
        if (gadget_mode() != 'A') { store_set("usb_mode", "A"); gadget_set_mode('A'); }
        buttons = 0; sticky = 0;
        logmsg("mode: %s", mode_name[input_mode]);
        ui_toast(mode_name[input_mode]);
    } else if (i == MI_PANEL) {
        if (gadget_mode() != 'B') { store_set("usb_mode", "B"); gadget_set_mode('B'); }
        logmsg("mode: PHONEBOX");
        ui_page_set(UI_PAGE_PANEL);
        ui_toast("PHONEBOX");
    } else if (i == MI_WEBCAM) {
        ui_toast("WEBCAM: NOT YET");
    } else if (i == MI_SETTINGS) {
        settings_open = 1; set_sel = 0;
    }
    redraw();
}

/* XCover short press: next top-level mode (skips the webcam placeholder). */
void hm_mode_step(int d) {
    int i = current_item();
    do i = (i + d + MI_WEBCAM) % MI_WEBCAM; while (i == MI_WEBCAM);
    menu_choose(i);
}

static int set_touch_id = -1, set_touch_item = -1;
static void settings_frame(const int *ids, const int *xs, const int *ys, int n) {
    int found = 0;
    for (int i = 0; i < n; i++) if (ids[i] == set_touch_id) found = 1;
    if (set_touch_id >= 0 && !found) {
        int it = set_touch_item;
        set_touch_id = set_touch_item = -1;
        if (it >= 0) set_step(it, +1);
        return;
    }
    if (set_touch_id < 0 && n > 0) {
        int it = (ys[0] - SET_Y0) / (SET_ITEM_H ? SET_ITEM_H : 1);
        set_touch_id = ids[0];
        set_touch_item = ys[0] >= SET_Y0 && it < SET_N ? it : -1;
        /* brightness: left half = down, handled by stepping -1 on release from the left */
        if (set_touch_item == 1 && xs[0] < fb->w / 2) { set_touch_item = -1; power_bright_step(-1); }
        if (set_touch_item >= 0) { set_sel = set_touch_item; }
        draw_settings(); fb_present(fb);
    }
}

static int menu_touch_id = -1, menu_touch_item = -1;
static void menu_frame(const int *ids, const int *xs, const int *ys, int n) {
    int found = 0;
    for (int i = 0; i < n; i++) if (ids[i] == menu_touch_id) { found = 1; (void)xs; }
    if (menu_touch_id >= 0 && !found) { /* lifted */
        int it = menu_touch_item;
        menu_touch_id = menu_touch_item = -1;
        if (it >= 0) { menu_choose(it); return; }
    }
    if (menu_touch_id < 0 && n > 0) {
        int it = (ys[0] - MENU_Y0) / (MENU_ITEM_H ? MENU_ITEM_H : 1);
        menu_touch_id = ids[0];
        menu_touch_item = ys[0] >= MENU_Y0 && it < MENU_N ? it : -1;
        if (menu_touch_item >= 0) { menu_sel = menu_touch_item; draw_menu(); fb_present(fb); }
    }
}

void hm_menu_toggle(void) {
    if (settings_open) { settings_open = 0; menu_open = 0; redraw(); return; }
    menu_open = !menu_open;
    if (menu_open) menu_sel = current_item();
    logmsg("input: menu %s", menu_open ? "open" : "closed");
    redraw();
}

/* Keys while the menu is open: vol/top = move, xcover/power = select. Returns 1 if consumed. */
int hm_menu_key(int code) {
    if (settings_open) {
        if (code == KEY_VOLUMEUP || code == KEY_MACRO17) set_sel = (set_sel + SET_N - 1) % SET_N;
        else if (code == KEY_VOLUMEDOWN) set_sel = (set_sel + 1) % SET_N;
        else if (code == 0xfc || code == KEY_POWER) { set_step(set_sel, +1); return 1; }
        else return 0;
        draw_settings(); fb_present(fb);
        return 1;
    }
    if (!menu_open) return 0;
    if (code == KEY_VOLUMEUP || code == KEY_MACRO17) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
    else if (code == KEY_VOLUMEDOWN) menu_sel = (menu_sel + 1) % MENU_N;
    else if (code == 0xfc || code == KEY_POWER) { menu_choose(menu_sel); return 1; }
    else return 0;
    draw_menu();
    fb_present(fb);
    return 1;
}

/* ---------------------------------------------------------------- glue */


static void redraw(void) {
    int o = hm_owner();
    gud_set_visible(o == OWN_GUD);
    if (o == OWN_MENU) { if (settings_open) draw_settings(); else draw_menu(); }
    else if (o == OWN_INPUT) { if (input_mode == IN_TRACKPAD) draw_trackpad(); else draw_keyboard(); }
    else if (o == OWN_PAGE) ui_page_set(ui_page());
    if (o != OWN_GUD) fb_present(fb);
    last_owner = o;
}

void hm_set_rotation(int deg) {
    if (!fb) return;
    fb_set_rotation(fb, deg);
    logmsg("ui: rotation %d (logical %dx%d)", fb->rot, fb->w, fb->h);
    last_owner = -1; /* force a full redraw in the new orientation */
    redraw();
}

void hm_init(struct fb *f) {
    fb = f;
    int r = store_get_int("rotation", -1);
    if (r > 0) fb_set_rotation(fb, r); /* auto (-1) keeps the current orientation until the sensor works */
    input_mode = store_get_int("input_mode", IN_TOUCHSCREEN);
    if (input_mode < 0 || input_mode >= IN_MODES) input_mode = IN_TOUCHSCREEN;
    memset(&tp, 0, sizeof tp);
}

/* Every main-loop iteration: notices ownership changes (e.g. the host enabling GUD). */
int hm_tick(void) {
    if (!fb || !fb->mem) return 1000;
    if (hm_owner() != last_owner) redraw();
    /* DEL auto-repeat while held */
    if (kb_down_row >= 0 && rows[kb_down_row][kb_down_col].code == 0x2a && now_ms() - kb_down_ms > 500 &&
        now_ms() - kb_repeat_ms >= 60) {
        kb_repeat_ms = now_ms();
        key_tap(0, 0x2a);
    }
    return kb_down_row >= 0 ? 30 : 1000;
}

/* A touch frame happened (touch.c state updated). Returns 1 if the frame was consumed
 * here (not to be forwarded as digitizer touches). */
int hm_touch_frame(void) {
    int ids[10], xs[10], ys[10];
    int n = touch_active_px(ids, xs, ys, 10);
    int o = hm_owner();
    if (o == OWN_PAGE && ui_page() == UI_PAGE_PANEL) { panel_touch(ids, xs, ys, n); return 1; }
    if (o == OWN_MENU) { if (settings_open) settings_frame(ids, xs, ys, n); else menu_frame(ids, xs, ys, n); return 1; }
    if (o != OWN_INPUT) return 0;
    if (input_mode == IN_TRACKPAD) {
        tp.y0 = 0; tp.y1 = fb->h; tp.with_buttons = 1;
        trackpad_frame(xs, ys, n);
    } else {
        tp.y0 = 0; tp.y1 = KB_TOP; tp.with_buttons = 0;
        keyboard_frame(ids, xs, ys, n);
        fb_present(fb);
    }
    return 1;
}

int hm_status(char *b, size_t len) {
    return snprintf(b, len, "input_mode %s\nmenu_open %d\nsettings_open %d\nowner %d\nbuttons %d\nsticky %d\nrotation %d\n",
                    mode_name[input_mode], menu_open, settings_open, hm_owner(), buttons, sticky, fb ? fb->rot : 0);
}

/* m = input mode (IN_TOUCHSCREEN..IN_COMBO), not a menu index (MI_PANEL is 0 there) */
void hm_set_mode(int m) { if (m >= 0 && m < IN_MODES) menu_choose(MI_SCREEN + m); }

/* The phonebox agent ran "am start" on us: show the PHONEBOX page (only if we're in that mode). */
void hm_show_phonebox(void) {
    if (gadget_mode() != 'B') return;
    menu_open = 0;
    settings_open = 0;
    ui_page_set(UI_PAGE_PANEL);
    redraw();
}
