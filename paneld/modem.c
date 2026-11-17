// WI-FI MODEM (M10b): the phone shares its Wi-Fi with the PC over the USB network (RNDIS/NCM, no driver on the
// PC). Picking it in the mode menu turns tethering on (NAT in wifi.c, the DHCP offer names the phone as router
// and DNS in gadget.c) and replugs USB so the PC takes the new lease at once; picking any other mode turns it
// off. This file draws its screen: what's shared, whether a PC is on the cable, live speed and totals.
#include "paneld.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BG 0x06080b
#define CARD 0x10141b
#define TEXT 0xeef2f6
#define MUTED 0x8a94a3
#define OK 0x22c55e
#define WARN 0xf59e0b
#define ACCENT 0x2dd4bf

static struct canvas cv, part;
static float S;
#define P(x) ((int)((x) * S + 0.5f))

static long stat_of(const char *ifn, const char *what) {
    char p[128], b[32];
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/%s", ifn, what);
    return read_file(p, b, sizeof b) > 0 ? atol(b) : -1;
}

/* The PC answered ARP on the USB network (it took the lease). */
static int pc_present(const char *ifn) {
    char b[2048];
    if (read_file("/proc/net/arp", b, sizeof b) <= 0) return 0;
    for (char *l = strchr(b, '\n'); l && *++l; l = strchr(l, '\n')) {
        char ip[32], hw[8], flags[8], mac[32], mask[8], dev[32];
        if (sscanf(l, "%31s %7s %7s %31s %7s %31s", ip, hw, flags, mac, mask, dev) == 6 && !strcmp(dev, ifn) &&
            strcmp(flags, "0x0")) return 1;
    }
    return 0;
}

/* Wi-Fi signal in dBm from /proc/net/wireless, or 0. */
static int wifi_dbm(void) {
    char b[512];
    if (read_file("/proc/net/wireless", b, sizeof b) <= 0) return 0;
    char *l = strstr(b, "wlan0:");
    float link, level;
    int st;
    if (!l || sscanf(l + 6, "%x %f %f", &st, &link, &level) != 3) return 0;
    return (int)level;
}

static void human_rate(double bps, char *o, size_t n) {
    if (bps < 1000) snprintf(o, n, "%.0f B/s", bps);
    else if (bps < 1e6) snprintf(o, n, "%.0f KB/s", bps / 1e3);
    else snprintf(o, n, "%.1f MB/s", bps / 1e6);
}

static void human_bytes(double b, char *o, size_t n) {
    if (b < 1e6) snprintf(o, n, "%.0f KB", b / 1e3);
    else if (b < 1e9) snprintf(o, n, "%.1f MB", b / 1e6);
    else snprintf(o, n, "%.2f GB", b / 1e9);
}

static struct { long rx, tx, ms; double down, up; long base_rx, base_tx; } st;

/* Samples the USB network once a second. "Down" is what the phone sends to the PC. */
static void sample(const char *ifn) {
    long rx = stat_of(ifn, "rx_bytes"), tx = stat_of(ifn, "tx_bytes"), now = now_ms();
    if (rx < 0 || tx < 0) { st.down = st.up = 0; st.ms = 0; return; }
    if (st.ms && now > st.ms && rx >= st.rx && tx >= st.tx) {
        double dt = (now - st.ms) / 1000.0;
        st.down = st.down * 0.4 + (tx - st.tx) / dt * 0.6;
        st.up = st.up * 0.4 + (rx - st.rx) / dt * 0.6;
    }
    if (!st.ms || rx < st.base_rx || tx < st.base_tx) { st.base_rx = rx; st.base_tx = tx; } /* counters reset */
    st.rx = rx; st.tx = tx; st.ms = now;
}

void modem_reset_totals(void) { memset(&st, 0, sizeof st); }

static int text_c(int font, int size, int cx, int y, const char *s, uint32_t c) {
    int w = cv_text_w(font, size, s, 0);
    return cv_text(&cv, font, size, cx - w / 2, y, s, c, 255, 0, 0);
}

/* Fills the canvas; returns the y range of the live numbers (for the once-a-second partial redraw). */
static void draw_all(int *live_y0, int *live_y1) {
    cv_clear(&cv, BG);
    int land = cv.w > cv.h, pad = P(16);
    char ssid[64] = "", ip[32] = "";
    int wifi_up = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
    int nat = wifi_tether_active();
    const char *ifn = gadget_net_if();
    int pc = gadget_mode() == 'B' && !strncmp(gadget_state(), "configured", 10) && pc_present(ifn);

    /* header */
    int ts = P(13), hy = P(30);
    cv_text(&cv, F_BOLD, ts, pad, hy, "WI-FI MODEM", MUTED, 255, ts * 0.16f, 0);

    /* the chain: Wi-Fi -> phone -> PC, one dot each */
    int top = P(52);
    int ch = land ? P(120) : P(150);
    cv_rrect(&cv, pad, top, cv.w - 2 * pad, ch, P(16), CARD, 255);
    const char *lab[3] = {"WI-FI", "PHONE", "PC"};
    char v0[64], v1[48], v2[48];
    int dbm = wifi_dbm();
    if (!ssid[0]) snprintf(v0, sizeof v0, "not set up");
    else if (!wifi_up) snprintf(v0, sizeof v0, "joining %.20s", ssid);
    else if (dbm) snprintf(v0, sizeof v0, "%.20s  %d dBm", ssid, dbm);
    else snprintf(v0, sizeof v0, "%.24s", ssid);
    snprintf(v1, sizeof v1, "%s", nat ? "sharing" : wifi_up ? "starting" : "waiting for Wi-Fi");
    snprintf(v2, sizeof v2, "%s", pc ? "online via phone" : gadget_mode() == 'B' && !strncmp(gadget_state(), "configured", 10) ? "connecting" : "plug in a PC");
    const char *vals[3] = {v0, v1, v2};
    int okc[3] = {wifi_up, nat, pc};
    int seg = (cv.w - 2 * pad) / 3;
    for (int k = 0; k < 3; k++) {
        int cx = pad + seg * k + seg / 2, dy = top + P(38);
        uint32_t c = okc[k] ? OK : k == 0 && !ssid[0] ? 0xef4444 : WARN;
        cv_circle(&cv, cx, dy, P(9), c, 60);
        cv_circle(&cv, cx, dy, P(5), c, 255);
        if (k < 2) cv_line(&cv, cx + P(18), dy, cx + seg - P(18), dy, P(2), okc[k] && okc[k + 1] ? OK : 0x3a414c, 255);
        text_c(F_BOLD, P(12), cx, dy + P(34), lab[k], TEXT);
        text_c(F_REGULAR, P(11), cx, dy + P(56), vals[k], MUTED);
    }

    /* live speed */
    int y0 = top + ch + P(12);
    *live_y0 = y0;
    int area = cv.h - y0 - P(44);
    int cw = land ? (cv.w - 2 * pad - P(12)) / 2 : cv.w - 2 * pad; /* side by side, or stacked upright */
    int lh = land ? area : (area - P(12)) / 2;
    const char *t2[2] = {"DOWNLOAD", "UPLOAD"};
    double r[2] = {st.down, st.up};
    double tot[2] = {(double)(st.tx - st.base_tx), (double)(st.rx - st.base_rx)};
    uint32_t col[2] = {ACCENT, 0x60a5fa};
    for (int k = 0; k < 2; k++) {
        int x = land ? pad + k * (cw + P(12)) : pad, yk = land ? y0 : y0 + k * (lh + P(12));
        cv_rrect(&cv, x, yk, cw, lh, P(16), CARD, 255);
        cv_text(&cv, F_BOLD, P(11), x + P(16), yk + P(26), t2[k], col[k], 255, P(11) * 0.14f, 0);
        char rs[32], ts2[32];
        human_rate(r[k], rs, sizeof rs);
        int big = land ? P(40) : P(34);
        while (big > P(14) && cv_text_w(F_LIGHT, big, rs, 0) > cw - P(32)) big--;
        cv_text(&cv, F_LIGHT, big, x + P(16), yk + P(26) + big + P(8), rs, TEXT, 255, 0, 0);
        human_bytes(tot[k] > 0 ? tot[k] : 0, ts2, sizeof ts2);
        char line[48];
        snprintf(line, sizeof line, "%s this session", ts2);
        cv_text(&cv, F_REGULAR, P(11), x + P(16), yk + lh - P(16), line, MUTED, 255, 0, cw - P(32));
    }
    *live_y1 = y0 + area;

    /* footer */
    char foot[160];
    if (!ssid[0]) snprintf(foot, sizeof foot, "Set up Wi-Fi from the PC: ppctl wifi set \"NAME\" \"PASSWORD\"");
    else if (land) snprintf(foot, sizeof foot, "Phone %s on Wi-Fi  -  PC gets 10.42.0.1  -  hold the side key for other modes", ip[0] ? ip : "-");
    else snprintf(foot, sizeof foot, "Phone %s  -  PC 10.42.0.1", ip[0] ? ip : "-");
    cv_text(&cv, F_REGULAR, P(10), pad, cv.h - P(16), foot, MUTED, 255, 0, cv.w - 2 * pad);
}

static struct fb *last_fb;
static int live0, live1;

void modem_draw(struct fb *f) {
    last_fb = f;
    if (cv_alloc(&cv, f->w, f->h) < 0) return;
    S = fminf(f->w, f->h) / 360.0f;
    sample(gadget_net_if());
    draw_all(&live0, &live1);
    fb_blit(f, &cv, 0, 0);
}

/* Once a second while the MODEM page shows: new numbers, and the chain when it changes. */
void modem_tick(struct fb *f) {
    if (f != last_fb || cv.w != f->w || cv.h != f->h) { modem_draw(f); fb_present(f); return; }
    sample(gadget_net_if());
    draw_all(&live0, &live1);
    /* the chain card is cheap to compare: blit everything above the live cards only when it changed */
    static uint32_t sum_prev;
    uint32_t sum = 0;
    for (int j = 0; j < live0; j += 3) for (int i = 0; i < cv.w; i += 7) sum = sum * 31 + cv.px[(size_t)j * cv.w + i];
    int y0 = sum != sum_prev ? 0 : live0, y1 = cv.h;
    sum_prev = sum;
    if (cv_alloc(&part, cv.w, y1 - y0) < 0) return;
    memcpy(part.px, cv.px + (size_t)y0 * cv.w, (size_t)cv.w * (y1 - y0) * 4);
    fb_blit(f, &part, 0, y0);
    fb_present(f);
}
