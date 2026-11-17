// paneld — shared declarations.
#pragma once
/* built with -D_GNU_SOURCE */
#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PP_VERSION
#define PP_VERSION "dev"
#endif
#ifndef PP_AUTOREBOOT_SECS
#define PP_AUTOREBOOT_SECS 0 /* dev safety net: reboot after this long with no input/host; 0 = never */
#endif

/* log.c / main.c */
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void do_reboot(const char *why, const char *target);
void do_poweroff(const char *why); /* target: NULL, "recovery", "download", ... */
void ensure_node(const char *cls, const char *name, const char *path);
long now_ms(void);
long now_us(void);
int write_file(const char *path, const char *s);
int read_file(const char *path, char *buf, size_t len); /* NUL-terminated, trailing newline stripped */

/* fb.c */
struct fb {
    int fd;
    uint8_t *mem;
    size_t len;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int w, h;        /* LOGICAL size (after rotation): all drawing uses these */
    int pw, ph;      /* PANEL size: GUD, screenshots, raw touch */
    int rot;         /* 0/90/180/270: UI turned clockwise relative to the panel */
    int bpp, stride;
    int blanked;
    long repan_until, repan_next; /* after unblank: re-present for a while (see fb_tick) */
    /* damage since the last present (empty when dx1 <= dx0) */
    int dx0, dy0, dx1, dy1;
    /* stats */
    unsigned long frames, px_total;
    int last_x, last_y, last_w, last_h;
};
int fb_blank(struct fb *f, int on);
int fb_tick(struct fb *f); /* main loop: re-presents after unblank; returns ms until next, or -1 */
void fb_set_rotation(struct fb *f, int deg); /* swaps w/h for 90/270; redraw afterwards */
int fb_present(struct fb *f);  /* flush iff something was drawn since last present; 1 = flushed */
void fb_damage_all(struct fb *f);
int fb_stats(struct fb *f, char *buf, size_t len);
void fb_damage_rect(struct fb *f, int x0, int y0, int w, int h); /* LOGICAL rect */

/* gfx.c — anti-aliased drawing (PHONEBOX look): canvases are 0xRRGGBB, logical orientation */
enum { F_LIGHT, F_REGULAR, F_MEDIUM, F_BOLD, F_MONO, GFX_FONTS };
struct canvas { int w, h; size_t cap; uint32_t *px; };
void gfx_init(void);
int cv_alloc(struct canvas *c, int w, int h);
void cv_clear(struct canvas *c, uint32_t rgb);
uint32_t rgb_mix(uint32_t a, uint32_t b, int t);
void cv_rect(struct canvas *c, int x, int y, int w, int h, uint32_t rgb, int a);
void cv_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t rgb, int a);
void cv_rrect_stroke(struct canvas *c, int x, int y, int w, int h, int r, float t, uint32_t rgb, int a);
void cv_circle(struct canvas *c, float cx, float cy, float r, uint32_t rgb, int a);
void cv_line(struct canvas *c, float x0, float y0, float x1, float y1, float th, uint32_t rgb, int a);
void cv_spark(struct canvas *c, int x, int y, int w, int h, const float *v, int n, uint32_t rgb, int line_a, int fill_a, float th);
int cv_text_w(int font, int size, const char *s, float ls);
int cv_text(struct canvas *c, int font, int size, int x, int y, const char *s, uint32_t rgb, int a, float ls, int max_w);
int cv_ascent(int font, int size);
int cv_image(struct canvas *out, const unsigned char *data, int len, int w, int h);
void cv_blit_rounded(struct canvas *c, const struct canvas *src, int x, int y, int r);
void fb_blit(struct fb *f, const struct canvas *c, int lx, int ly);
void fb_stats_reset(struct fb *f);
int fb_text(struct fb *f, int x, int y, int scale, const char *s, uint32_t fg, uint32_t bg);
int fb_open(struct fb *f, const char *path);
uint32_t fb_color(const struct fb *f, uint8_t r, uint8_t g, uint8_t b);
void fb_fill(struct fb *f, int x, int y, int w, int h, uint32_t c);
void fb_flush(struct fb *f);
void draw_test_pattern(struct fb *f);
void draw_clock(struct fb *f, const char *text);
void draw_status(struct fb *f, const char *text);

/* input.c */
#define MAX_INPUTS 16
struct input {
    int fd;
    char name[64];
    char node[16];
    int is_touch, has_power;
    int absmax_x, absmax_y;
    int slot, x[10], y[10];
};
int inputs_open(struct input *in, int max);
int inputs_scan(struct input *in, int n, int max); /* add newly appeared devices */

/* touch.c — multitouch tracking/painting, rotation, latency stats, injection */
struct input_event;
void touch_init(struct fb *f);
void touch_use_boottime(int evfd);
int touch_event(const struct input *dev, const struct input_event *e); /* 1 = flush needed */
void touch_flushed(const struct input_event *syn);
int touch_stats(char *buf, size_t len);
void touch_stats_reset(void);
void touch_set_rotation(int deg);
int touch_rotation(void);
void panel_to_logical(int px, int py, int *lx, int *ly);
void logical_to_panel(int lx, int ly, int *px, int *py);
int inject_open(void);
int inject_stroke(int slot, const int *xs, const int *ys, int n, int dt_ms);
int inject_multi(int k, int y0, int y1, int steps);
int inject_key_open(void);              /* create the virtual key device (parent) */
int inject_key(int code, int hold_ms); /* press it (child) */
/* HID digitizer contacts for the current frame (incl. contacts just lifted); returns count */
int touch_hid_contacts(int *ids, int *xs, int *ys, int *down, int max);
int touch_corner_hold(int ms);   /* 1 once when a top-left long-press completes */
int touch_corner_pending(void);

/* store.c — persistent key=value settings in the tail of phonepanel's partition */
int store_init(const char *partition);
const char *store_get(const char *key, const char *dflt);
void store_set_safe_mode(int on);
int store_safe_mode(void);
int boot_key_held(int code);            /* util.c: is this key down right now (any input device)? */
int store_get_int(const char *key, int dflt);
int store_set(const char *key, const char *value);
int store_save(void);
int store_dump(char *buf, size_t len);

/* power.c — screen states, backlight, CPU caps, charge limiter, thermal guard */
struct power_cfg {
    int dim_after_s, off_after_s, bright, dim;
    int charge_lo, charge_hi, temp_limit, temp_crit; /* %, %, 0.1 °C, 0.1 °C */
    int cap_cpu_when_off, limiter;
};
extern struct power_cfg pcfg;
void power_init(struct fb *f);
void power_user_activity(void);
void power_toggle(void);
void power_off(const char *why);
void power_wake(const char *why);
void power_bright_step(int dir);
void power_set_percent(int pct); /* 0..100 perceptual (GUD backlight property) */
int power_tick(void); /* returns ms until the next deadline */
int power_screen_off(void);
void power_dim(const char *why);   /* ACTIVE -> DIM (backlight to the dim level); input brightens again */
int power_ctl(char **argv, int argc, char *buf, size_t len);

/* keys.c — hardware keys -> configurable short/long-press actions */
void keys_init(void);
void keys_event(int code, int value);
int keys_tick(void); /* ms until the next key deadline (power-hold reboot) */
void key_action(const char *act);
int keys_dump(char *buf, size_t len);

/* ui.c — built-in pages until the widget engine (M8) */
enum { UI_PAGE_TEST, UI_PAGE_STATUS, UI_PAGE_PANEL, UI_PAGES };
void ui_init(struct fb *f);
int ui_page(void);
void ui_page_set(int p);
void ui_page_step(int d);
void ui_toast(const char *msg);
void ui_tick(void);

/* gud.c — GUD USB display device (mode A) */
int gud_init(struct fb *f, const char *configfs_gadget);
int gud_ready(void);
int gud_active(void);   /* host owns the screen */
int gud_stats(char *b, size_t len);
void gud_stats_reset(void);
void gud_set_workers(int n);
void gud_set_visible(int v); /* hidden: frames go to a shadow copy, restored on show */

/* hid_modes.c — phone modes (PANEL/SCREEN/MOUSE/KEYBOARD/COMBO), menu, screen ownership */
enum { IN_TOUCHSCREEN, IN_TRACKPAD, IN_KEYBOARD, IN_COMBO, IN_RESCUE, IN_MODES };
enum { OWN_PAGE, OWN_GUD, OWN_INPUT, OWN_MENU };
void hm_init(struct fb *f);
int hm_owner(void);
int hm_input_mode(void);
int hm_menu_open(void);
void hm_menu_toggle(void);
int hm_menu_key(int code);
void hm_mode_step(int d);
void hm_set_mode(int m);
void hm_set_rotation(int deg);
int hm_tick(void);
int hm_touch_frame(void);
int hm_status(char *b, size_t len);
int touch_active_px(int *ids, int *xs, int *ys, int max);

/* sensors.c — sensor hub (CHUB) and autorotate */
void sensors_set_running(int on);
int sensors_fd(int which);   /* 0 accelerometer, 1 hub orientation; -1 if not open */
void sensors_read(int which);
int sensors_stats(char *buf, size_t len);

/* json.c — minimal tokenizer */
enum { J_OBJ, J_ARR, J_STR, J_PRIM };
typedef struct { int type, start, end, size, parent; } jtok;
int json_parse(const char *s, int len, jtok *t, int max);
int json_skip(const jtok *t, int n, int i);
int json_get(const char *s, const jtok *t, int n, int obj, const char *key);
int json_str(const char *s, const jtok *t, int i, char *out, size_t len);
double json_num(const char *s, const jtok *t, int i, double dflt);

void hm_show_phonebox(void);
void hm_key_tap(int mods, int code); /* one key press + release on the USB HID keyboard */       /* switch to the PHONEBOX page (phonebox agent "am start") */

/* panel.c — PPJ/1 widget engine (PANEL mode) */
void panel_init(struct fb *f);
int panel_handle(const char *line, int len, char *reply, size_t rlen); /* reply: JSON line or "" */
int panel_has_layout(void);
void panel_draw(void);                 /* full redraw of the current panel page */
int panel_touch(const int *ids, const int *xs, const int *ys, int n); /* 1 = consumed */
int panel_page_step(int d);           /* 0 = moved within panel pages, 1 = wrapped out */
void panel_tick(void);                /* once per second: clocks */
void panel_set_event_sink(void (*fn)(const char *json));
int panel_landscape(void);
int net_init(void);                    /* TCP server for PPJ/1 */
int net_fd(int which);                 /* 0 listen, 1 client */
void net_handle(int which);

/* gadget.c */
/* Creates the gadget with GADGET_PORTS ACM functions; fills ttys[i] ("" if that port is absent).
 * Only the control port is required. */
enum { GADGET_LOG, GADGET_CTL, GADGET_SH, GADGET_PORTS };
int gadget_setup(char ttys[GADGET_PORTS][64], struct fb *f);
int gadget_set_mode(char m);         /* 'A' display (HID touch [+GUD]), 'B' smart (NCM) */
char gadget_mode(void);
void gadget_tick(void);
void gadget_dhcp_refresh(void);     /* re-offer DHCP to the host (tethering changed) */
void run_quiet(char *const argv[]);

/* wifi.c — Wi-Fi client (wpa_supplicant + udhcpc) for the network in the settings store */
void wifi_tick(void);

/* ssh.c — dropbear, key-only, only with authorised keys (M11b) */
void ssh_tick(void);
void bt_tick(void);                    /* bt.c: SCSC raw HCI <-> vhci bridge when enabled (M10c) */
int bt_ctl(char **argv, int argc, char *out, size_t len);
int ssh_ctl(char **argv, int argc, const char *raw, char *out, size_t len);
void term_tick(void);                  /* term.c: hand the console to/from a remote (ssh) session */
int wifi_ctl(char **argv, int argc, char *out, size_t len); /* -2: "scan" (ctl runs iw) */  /* fork+exec, output to /dev/null, wait */

/* adbd.c — minimal adb device (PHONEBOX mode: the phonebox agent drives us over adb) */
int adbd_init(const char *configfs_gadget);
int adbd_ready(void);
int adbd_notify_fd(void);
int adbd_notify_read(void);
int adbd_stats(char *b, size_t len);
int adbd_reverse_active(int port);

/* pbclient.c — PHONEBOX mode: native client of the phonebox desktop agent (through adb reverse) */
void pb_init(struct fb *f);
int pb_fd(void);
void pb_handle(void);
void pb_tick(void);
int pb_owns(void);                    /* phonebox pages are shown (config received, no PPJ/1 layout active) */
void pb_draw(void);
int pb_touch(const int *ids, const int *xs, const int *ys, int n);
int pb_page_step(int d);
int pb_page_set(int n);
int pb_stats(char *b, size_t len);
int pb_hits(char *b, size_t len);              /* mode B self-heal (once a second) */
const char *gadget_state(void);      /* UDC state, "configured" when a host is attached */
int gadget_reconnect(int off_ms);    /* soft disconnect/connect */
int gadget_hid_report(uint8_t *buf, const int *ids, const int *xs, const int *ys, const int *down, int n);

/* util.c — mirror log lines to the read-only log port */
void log_port_open(const char *tty);
int log_port_fd(void);
void log_port_drain(void); /* discard whatever the host writes into the log port */
void log_port_reopen(void); /* after a hangup */

/* shell.c — busybox shell on the second ACM port, respawned when it exits */
void shell_setup(void);                 /* busybox --install, if /bin/busybox exists */
void shell_start(const char *tty);
void shell_reap(void);
void shell_pause(int on);              /* RESCUE: stop the debug shell and keep its port free (1), or bring it back (0) */

/* term.c — RESCUE mode: terminal for a server's serial console on the shell port */
void term_init(struct fb *f);
void term_activate(int on, const char *console_tty);
int term_active(void);
int term_fd(void);
void term_read(void);
void term_draw(void);
void term_toggle_keyboard(void);
int term_touch(const int *ids, const int *xs, const int *ys, int n);
int term_stats(char *b, size_t len);
void term_type(const char *s);                  /* call often: reaps all children (we are PID 1) */

/* ctl.c — control channel on the ACM tty */
struct app {
    struct fb *fb;
    int have_fb;
    struct input *in;
    int nin;
    long last_activity_ms;
    int idle_reboot_s;
    long boot_ms; /* CLOCK_BOOTTIME when paneld started */
    int clock_on; /* 1 Hz status clock */
};
int ctl_open(const char *tty);
void ctl_reopen(void); /* after a hangup (USB disconnect / mode switch) */
int ctl_fd(void);
int ctl_handle(struct app *a); /* call when ctl_fd() is readable; <0 = hung up, reopen */

/* watchdog.c — reboots (into the ROM) if the main loop stops making progress,
 * after dumping where it is stuck into the kernel log. */
void wd_start(void);
void wd_beat(const char *phase);           /* main loop is alive, now doing <phase> */
void wd_phase(const char *phase);          /* label the current step without counting it as progress */
void wd_allow(const char *phase, int secs); /* <phase> may legitimately take up to secs */
