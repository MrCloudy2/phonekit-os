// Built-in pages until the widget engine (M8): the test/touch page and a status page.
// Keys cycle pages (keys.c); ui_tick redraws the status page once per second.
#include "paneld.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static struct fb *fb;
static int page = UI_PAGE_TEST;
static long toast_until;
static const char *const page_name[UI_PAGES] = {"TEST", "STATUS"};

void ui_init(struct fb *f) { fb = f; }
int ui_page(void) { return page; }

static void draw_status_page(void) {
    uint32_t bg = fb_color(fb, 10, 14, 22), fg = fb_color(fb, 230, 235, 245), dim = fb_color(fb, 130, 140, 160);
    static int drawn_bg;
    if (!drawn_bg) { fb_fill(fb, 0, 0, fb->w, fb->h, bg); drawn_bg = 1; }
    char l[64];
    int y = 140;
    fb_text(fb, 60, y, 9, "PHONEPANEL", fg, bg); y += 140;
    snprintf(l, sizeof l, "USB %c %-14.14s", gadget_mode() ? gadget_mode() : '-', gadget_state());
    fb_text(fb, 60, y, 6, l, fg, bg); y += 90;
    snprintf(l, sizeof l, "%-20s", gadget_mode() == 'B' ? "IP 10.42.0.2" : "HID TOUCHSCREEN");
    fb_text(fb, 60, y, 6, l, dim, bg); y += 120;
    char cap[16] = "?", temp[16] = "?", st[24] = "?";
    read_file("/sys/class/power_supply/battery/capacity", cap, sizeof cap);
    read_file("/sys/class/power_supply/battery/batt_temp", temp, sizeof temp);
    read_file("/sys/class/power_supply/battery/status", st, sizeof st);
    int t = 0;
    sscanf(temp, "%d", &t);
    snprintf(l, sizeof l, "BATTERY %3s%%  %2d.%dC   ", cap, t / 10, t % 10);
    fb_text(fb, 60, y, 6, l, fg, bg); y += 90;
    snprintf(l, sizeof l, "%-20.20s", st);
    fb_text(fb, 60, y, 6, l, dim, bg); y += 120;
    long up = now_ms() / 1000;
    snprintf(l, sizeof l, "UP %ldD %02ld:%02ld:%02ld ", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60);
    fb_text(fb, 60, y, 6, l, fg, bg); y += 160;
    fb_text(fb, 60, y, 4, "XCOVER: NEXT MODE  HOLD: MENU", dim, bg); y += 50;
    fb_text(fb, 60, y, 4, "TOP: PAGES  VOL: BRIGHTNESS", dim, bg); y += 50;
    fb_text(fb, 60, y, 4, "POWER: SCREEN  HOLD 3 S: REBOOT", dim, bg);
}

static void draw_page(void) {
    if (page == UI_PAGE_TEST) draw_test_pattern(fb);
    else {
        uint32_t bg = fb_color(fb, 10, 14, 22);
        fb_fill(fb, 0, 0, fb->w, fb->h, bg);
        draw_status_page();
    }
}

void ui_page_set(int p) {
    if (!fb || !fb->mem) return;
    page = ((p % UI_PAGES) + UI_PAGES) % UI_PAGES;
    logmsg("ui: page %s", page_name[page]);
    draw_page();
    fb_present(fb);
}

void ui_page_step(int d) { ui_page_set(page + d); }

/* Short banner at the bottom (e.g. after a mode switch), cleared after 2 s. */
void ui_toast(const char *msg) {
    if (!fb || !fb->mem) return;
    uint32_t bg = fb_color(fb, 40, 90, 160), fg = fb_color(fb, 255, 255, 255);
    fb_fill(fb, 0, fb->h - 170, fb->w, 110, bg);
    fb_text(fb, 40, fb->h - 150, 8, msg, fg, bg);
    fb_present(fb);
    toast_until = now_ms() + 2000;
}

/* Once per second from the main loop: page updates + toast expiry. */
void ui_tick(void) {
    if (!fb || !fb->mem) return;
    if (toast_until && now_ms() >= toast_until) { toast_until = 0; draw_page(); }
    if (page == UI_PAGE_STATUS) draw_status_page();
}
