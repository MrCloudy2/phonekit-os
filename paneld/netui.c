// Settings -> WI-FI and Settings -> BLUETOOTH: the on-phone network screens.
//
// WI-FI: status, Wi-Fi on/off, "keep off while wired" (default on), SCAN (iw, in a thread so the main loop never
// waits), tap a network -> on-screen keyboard for the password (hidden by default, SHOW to check it) -> CONNECT,
// FORGET. BLUETOOTH: status, on/off, "keep off while wired" (default on), PAIR (discoverable 3 min), CONNECT to the
// paired host, FORGET it. Both default to off unless the user turns them on: nothing runs that isn't needed.
#include "paneld.h"

#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define BG 0x06080b
#define CARD 0x11151c
#define LINE 0x232a35
#define TEXT 0xeef2f6
#define MUTED 0x8a94a3
#define ACCENT 0x2dd4bf
#define OKC 0x22c55e
#define WARN 0xf59e0b
#define BAD 0xef4444

static struct fb *fb;
static struct canvas cv;
static float S;
#define P(x) ((int)((x) * S + 0.5f))

static int screen;        /* NET_NONE, NET_WIFI, NET_BT */
static int pass_open;     /* the password keyboard is showing */
static char msg[96];      /* one line of feedback ("joining...", errors) */
static long msg_until;

/* ---------------------------------------------------------------- hit boxes */

enum { H_BACK = 1, H_TOGGLE_ON, H_TOGGLE_WIRED, H_SCAN, H_FORGET, H_NET0 = 100, H_KEY0 = 300,
       H_PAIR = 500, H_CONNECT, H_SHOW, H_CANCEL, H_JOIN };
#define MAXHIT 128
static struct { int id, x, y, w, h; } hits[MAXHIT];
static int nhit;
static void hit(int id, int x, int y, int w, int h) { if (nhit < MAXHIT) hits[nhit++] = (typeof(hits[0])){id, x, y, w, h}; }
static int hit_at(int x, int y) {
    for (int i = nhit - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) return hits[i].id;
    return 0;
}

/* ---------------------------------------------------------------- scan (thread) */

#define MAXNET 24
struct net { char ssid[33]; int dbm, secure, sae_only; };
static struct net nets[MAXNET];
static int nnet;
static volatile int scanning, scan_done;
static long scanned_at;
static char scan_err[64];

static void parse_scan(char *out) {
    struct net cur = {0};
    int have = 0, psk = 0, sae = 0;
    nnet = 0;
    for (char *l = strtok(out, "\n");; l = strtok(NULL, "\n")) {
        int end = !l;
        if (end || !strncmp(l, "BSS ", 4)) {
            if (have && cur.ssid[0]) {
                cur.sae_only = sae && !psk;
                int k;
                for (k = 0; k < nnet && strcmp(nets[k].ssid, cur.ssid); k++) {}
                if (k < nnet) { if (cur.dbm > nets[k].dbm) nets[k] = cur; }
                else if (nnet < MAXNET) nets[nnet++] = cur;
            }
            if (end) break;
            memset(&cur, 0, sizeof cur);
            have = 1; psk = sae = 0;
            continue;
        }
        while (*l == '\t' || *l == ' ') l++;
        if (!strncmp(l, "signal:", 7)) cur.dbm = (int)atof(l + 7);
        else if (!strncmp(l, "SSID:", 5)) {
            const char *v = l + 5;
            while (*v == ' ') v++;
            snprintf(cur.ssid, sizeof cur.ssid, "%s", v);
            if (strstr(cur.ssid, "\\x00")) cur.ssid[0] = 0; /* hidden network */
        } else if (!strncmp(l, "RSN:", 4) || !strncmp(l, "WPA:", 4)) cur.secure = 1;
        else if (strstr(l, "Authentication suites:")) { if (strstr(l, "PSK")) psk = 1; if (strstr(l, "SAE")) sae = 1; }
    }
    /* strongest first */
    for (int i = 0; i < nnet; i++)
        for (int j = i + 1; j < nnet; j++)
            if (nets[j].dbm > nets[i].dbm) { struct net t = nets[i]; nets[i] = nets[j]; nets[j] = t; }
}

static int run_capture(char *const argv[], char *out, size_t len) {
    int pf[2];
    if (pipe2(pf, O_CLOEXEC) < 0) return -1;
    pid_t p = fork();
    if (p == 0) {
        dup2(pf[1], 1);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        execv(argv[0], argv);
        _exit(127);
    }
    close(pf[1]);
    size_t o = 0;
    ssize_t n;
    while (o + 1 < len && (n = read(pf[0], out + o, len - 1 - o)) > 0) o += (size_t)n;
    out[o] = 0;
    close(pf[0]);
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void *scan_thread(void *v) {
    (void)v;
    static char out[65536];
    char *up[] = {"/bin/busybox", "ifconfig", "wlan0", "up", NULL};
    run_capture(up, out, sizeof out); /* powers the chip (loads its firmware) if Wi-Fi was off */
    scan_err[0] = 0;
    int rc = -1;
    for (int tries = 0; tries < 3 && rc != 0; tries++) { /* "busy" while wpa_supplicant scans: wait and retry */
        char *sc[] = {"/bin/iw", "dev", "wlan0", "scan", NULL};
        rc = run_capture(sc, out, sizeof out);
        if (rc != 0) usleep(1500000);
    }
    if (rc != 0) { /* fall back to what the chip saw last */
        char *dump[] = {"/bin/iw", "dev", "wlan0", "scan", "dump", NULL};
        rc = run_capture(dump, out, sizeof out);
    }
    if (rc == 0) parse_scan(out);
    else snprintf(scan_err, sizeof scan_err, "scan failed");
    scanned_at = now_ms();
    scanning = 0;
    scan_done = 1;
    return NULL;
}

static void start_scan(void) {
    if (scanning || access("/bin/iw", X_OK) != 0) { if (!scanning) snprintf(scan_err, sizeof scan_err, "no iw in this image"); return; }
    scanning = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread, NULL)) { scanning = 0; return; }
    pthread_detach(t);
}

/* ---------------------------------------------------------------- password entry */

static char sel_ssid[33];
static char pw[64];
static int pw_show, kb_shift, kb_sym;

static const char *const rows_lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *const rows_sym[] = {"1234567890", "!@#$%^&*()", "-_=+[]{}:;", "'/,.?<>|~`"};

/* ---------------------------------------------------------------- drawing helpers */

static void text_mid(int font, int size, int x, int y, int w, int h, const char *s, uint32_t c) {
    int tw = cv_text_w(font, size, s, 0);
    if (tw > w - P(8)) tw = w - P(8);
    cv_text(&cv, font, size, x + (w - tw) / 2, y + h / 2 + (int)(size * 0.36f), s, c, 255, 0, w - P(8));
}

static void button(int id, int x, int y, int w, int h, const char *label, uint32_t fill, uint32_t tc) {
    cv_rrect(&cv, x, y, w, h, P(10), fill, 255);
    text_mid(F_BOLD, P(12), x, y, w, h, label, tc);
    hit(id, x, y, w, h);
}

static void toggle_row(int id, int x, int y, int w, int h, const char *label, const char *sub, int on) {
    cv_rrect(&cv, x, y, w, h, P(12), CARD, 255);
    cv_text(&cv, F_MEDIUM, P(13), x + P(14), y + h / 2 - (sub ? P(2) : -P(5)), label, TEXT, 255, 0, w - P(90));
    if (sub) cv_text(&cv, F_REGULAR, P(10), x + P(14), y + h / 2 + P(13), sub, MUTED, 255, 0, w - P(90));
    int sw = P(46), sh = P(26), sx = x + w - sw - P(14), sy = y + (h - sh) / 2;
    cv_rrect(&cv, sx, sy, sw, sh, sh / 2, on ? ACCENT : 0x39414d, 255);
    cv_circle(&cv, on ? sx + sw - sh / 2 : sx + sh / 2, sy + sh / 2, sh / 2 - P(3), 0xffffff, 255);
    hit(id, x, y, w, h);
}

static void header(const char *title) {
    int hh = P(46);
    cv_rect(&cv, 0, 0, cv.w, hh, BG, 255);
    int ts = P(13), base = hh / 2 + (int)(ts * 0.38f);
    int bw = cv_text_w(F_MEDIUM, ts, "< SETTINGS", 0) + P(28);
    cv_rrect(&cv, P(8), hh / 2 - P(15), bw, P(30), P(15), 0xffffff, 22);
    cv_text(&cv, F_MEDIUM, ts, P(22), base, "< SETTINGS", TEXT, 255, 0, 0);
    hit(H_BACK, 0, 0, bw + P(16), hh);
    cv_text(&cv, F_BOLD, ts, bw + P(30), base, title, MUTED, 255, ts * 0.16f, 0);
}

static void bars(int x, int y, int dbm, uint32_t c) {
    int lvl = dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
        int bh = P(4) + b * P(3);
        cv_rect(&cv, x + b * P(5), y - bh, P(3), bh, b < lvl ? c : 0x39414d, 255);
    }
}

static void lock_icon(int x, int y, uint32_t c) {
    cv_rrect_stroke(&cv, x + P(2), y - P(13), P(8), P(9), P(4), fmaxf(1.5f, S * 0.7f), c, 255);
    cv_rrect(&cv, x, y - P(8), P(12), P(9), P(2), c, 255);
}

static void say(const char *s) { snprintf(msg, sizeof msg, "%s", s); msg_until = now_ms() + 6000; }

/* ---------------------------------------------------------------- WI-FI screen */

static void wifi_status_text(char *a, size_t al, char *b, size_t bl, uint32_t *dot) {
    char ssid[64] = "", ip[32] = "";
    int up = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
    if (!ssid[0]) { snprintf(a, al, "Not set up"); snprintf(b, bl, "Scan and pick a network"); *dot = MUTED; return; }
    if (!store_get_int("wifi.on", 1)) { snprintf(a, al, "Off"); snprintf(b, bl, "Saved: %.32s", ssid); *dot = MUTED; return; }
    if (!wifi_wanted()) { snprintf(a, al, "Off while wired"); snprintf(b, bl, "Comes on when the cable is out (%.24s)", ssid); *dot = MUTED; return; }
    if (up && wifi_running()) { snprintf(a, al, "Connected to %.32s", ssid); snprintf(b, bl, "%s", ip); *dot = OKC; return; }
    snprintf(a, al, "Joining %.32s", ssid); snprintf(b, bl, "Wrong password? Tap the network again"); *dot = WARN;
}

static void draw_net_list(int x, int y, int w, int h) {
    cv_text(&cv, F_BOLD, P(11), x + P(4), y + P(14), "NETWORKS", MUTED, 255, P(11) * 0.14f, 0);
    int ly = y + P(26), rh = P(44);
    char cur[64] = "", ip[32];
    wifi_status(cur, sizeof cur, ip, sizeof ip);
    if (scanning) { cv_text(&cv, F_REGULAR, P(12), x + P(4), ly + P(26), "Scanning...", MUTED, 255, 0, 0); return; }
    if (!nnet) {
        cv_text(&cv, F_REGULAR, P(12), x + P(4), ly + P(26), scan_err[0] ? scan_err : scanned_at ? "No networks found" : "Tap SCAN to look for networks",
                MUTED, 255, 0, w);
        return;
    }
    for (int i = 0; i < nnet && ly + rh <= y + h; i++, ly += rh + P(6)) {
        const struct net *n = &nets[i];
        int is_cur = !strcmp(n->ssid, cur);
        cv_rrect(&cv, x, ly, w, rh, P(10), is_cur ? 0x10302b : CARD, 255);
        bars(x + P(14), ly + rh / 2 + P(8), n->dbm, is_cur ? ACCENT : TEXT);
        cv_text(&cv, F_MEDIUM, P(13), x + P(44), ly + rh / 2 + P(5), n->ssid, TEXT, 255, 0, w - P(110));
        if (n->sae_only) cv_text(&cv, F_REGULAR, P(9), x + w - P(78), ly + rh / 2 + P(4), "WPA3 ONLY", WARN, 255, 0, 0);
        else if (n->secure) lock_icon(x + w - P(28), ly + rh / 2 + P(7), MUTED);
        hit(H_NET0 + i, x, ly, w, rh);
    }
}

static void draw_wifi(void) {
    header("WI-FI");
    int land = cv.w > cv.h, pad = P(12), top = P(52);
    int lw = land ? (cv.w - 3 * pad) * 45 / 100 : cv.w - 2 * pad;
    int x = pad, y = top;
    char a[80], b[80];
    uint32_t dot;
    wifi_status_text(a, sizeof a, b, sizeof b, &dot);
    int sh = P(64);
    cv_rrect(&cv, x, y, lw, sh, P(12), CARD, 255);
    cv_circle(&cv, x + P(20), y + sh / 2, P(6), dot, 255);
    cv_text(&cv, F_MEDIUM, P(14), x + P(36), y + sh / 2 - P(3), a, TEXT, 255, 0, lw - P(48));
    cv_text(&cv, F_REGULAR, P(10), x + P(36), y + sh / 2 + P(15), b, MUTED, 255, 0, lw - P(48));
    y += sh + P(8);
    int th = P(52);
    toggle_row(H_TOGGLE_ON, x, y, lw, th, "Wi-Fi", NULL, store_get_int("wifi.on", 1));
    y += th + P(8);
    toggle_row(H_TOGGLE_WIRED, x, y, lw, th, "Keep off while wired", "Saves power; WI-FI MODEM keeps it on", store_get_int("wifi.wired_off", 1));
    y += th + P(8);
    int bw = (lw - P(8)) / 2, bh = P(44);
    button(H_SCAN, x, y, bw, bh, scanning ? "SCANNING..." : "SCAN", 0x1d4f4a, TEXT);
    if (store_get("wifi.ssid", "")[0]) button(H_FORGET, x + bw + P(8), y, bw, bh, "FORGET", 0x3a1d22, 0xfca5a5);
    y += bh + P(8);
    if (msg[0] && now_ms() < msg_until) cv_text(&cv, F_REGULAR, P(11), x + P(4), y + P(14), msg, WARN, 255, 0, lw);
    if (land) draw_net_list(x + lw + pad, top, cv.w - lw - 3 * pad, cv.h - top - pad);
    else draw_net_list(x, y + P(24), lw, cv.h - y - P(24) - pad);
}

static void draw_pass(void) {
    header("WI-FI PASSWORD");
    int pad = P(12), y = P(52), w = cv.w - 2 * pad;
    char t[80];
    snprintf(t, sizeof t, "Password for %s", sel_ssid);
    cv_text(&cv, F_MEDIUM, P(13), pad + P(4), y + P(14), t, TEXT, 255, 0, w);
    y += P(24);
    int fh = P(44);
    cv_rrect(&cv, pad, y, w, fh, P(10), CARD, 255);
    cv_rrect_stroke(&cv, pad, y, w, fh, P(10), fmaxf(1.5f, S * 0.7f), ACCENT, 160);
    char shown[72];
    size_t n = strlen(pw);
    if (pw_show) snprintf(shown, sizeof shown, "%s", pw);
    else { for (size_t i = 0; i < n && i < sizeof shown - 1; i++) shown[i] = '*'; shown[n < sizeof shown ? n : sizeof shown - 1] = 0; }
    int tx = cv_text(&cv, F_MEDIUM, P(15), pad + P(14), y + fh / 2 + P(6), shown, TEXT, 255, 0, w - P(28));
    cv_rect(&cv, tx + P(2), y + P(10), P(2), fh - P(20), ACCENT, 255); /* caret */
    y += fh + P(8);
    if (msg[0] && now_ms() < msg_until) { cv_text(&cv, F_REGULAR, P(11), pad + P(4), y + P(10), msg, WARN, 255, 0, w); }
    y += P(16);
    /* keyboard: 4 rows of keys + 1 row of actions */
    int kb_h = cv.h - y - pad, rows = 5, gap = P(5);
    int kh = (kb_h - (rows - 1) * gap) / rows;
    const char *const *R = kb_sym ? rows_sym : rows_lower;
    for (int r = 0; r < 4; r++) {
        const char *keys = R[r];
        int nk = (int)strlen(keys), extra = (!kb_sym && r == 3) ? 2 : 0; /* shift + backspace on the last letter row */
        if (kb_sym && r == 3) extra = 1;                                 /* backspace */
        int slots = 10;
        int kw = (w - (slots - 1) * gap) / slots;
        int row_w = nk * kw + (nk - 1) * gap + extra * (kw * 3 / 2 + gap);
        int x = pad + (w - row_w) / 2;
        if (!kb_sym && r == 3) {
            button(H_KEY0 + 1, x, y, kw * 3 / 2, kh, kb_shift ? "SHIFT ON" : "SHIFT", kb_shift ? 0x1d4f4a : 0x1a1f28, TEXT);
            x += kw * 3 / 2 + gap;
        }
        for (int k = 0; k < nk; k++) {
            char c = keys[k];
            if (!kb_sym && kb_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            char s[2] = {c, 0};
            cv_rrect(&cv, x, y, kw, kh, P(8), 0x1f2530, 255);
            text_mid(F_MEDIUM, P(16), x, y, kw, kh, s, TEXT);
            hit(H_KEY0 + (unsigned char)c, x, y, kw, kh); /* printable ASCII: H_KEY0 + 32..126 */
            x += kw + gap;
        }
        if (r == 3) button(H_KEY0 + 2, x, y, kw * 3 / 2, kh, "DEL", 0x1a1f28, TEXT);
        y += kh + gap;
    }
    /* actions */
    int aw = (w - 4 * gap) / 5;
    int x = pad;
    button(H_KEY0 + 3, x, y, aw, kh, kb_sym ? "ABC" : "?123", 0x1a1f28, TEXT); x += aw + gap;
    button(H_KEY0 + ' ', x, y, aw, kh, "SPACE", 0x1f2530, TEXT); x += aw + gap;
    button(H_SHOW, x, y, aw, kh, pw_show ? "HIDE" : "SHOW", 0x1a1f28, TEXT); x += aw + gap;
    button(H_CANCEL, x, y, aw, kh, "CANCEL", 0x3a1d22, 0xfca5a5); x += aw + gap;
    button(H_JOIN, x, y, aw, kh, "CONNECT", 0x14532d, 0xbbf7d0);
}

/* ---------------------------------------------------------------- BLUETOOTH screen */

static void draw_bt(void) {
    header("BLUETOOTH");
    int land = cv.w > cv.h, pad = P(12), top = P(52);
    int lw = land ? (cv.w - 3 * pad) / 2 : cv.w - 2 * pad;
    int x = pad, y = top;
    const char *st = bthid_status();
    char a[80], b[120];
    uint32_t dot = MUTED;
    int paired = store_get("bt.keys", "")[0] != 0;
    if (!store_get_int("bt", 0)) { snprintf(a, sizeof a, "Off"); snprintf(b, sizeof b, "Turn it on to use KEYBOARD, MOUSE or DECK over Bluetooth"); }
    else if (!bt_wanted()) { snprintf(a, sizeof a, "Off while wired"); snprintf(b, sizeof b, "Comes on when the cable is out"); }
    else if (bthid_connected()) { snprintf(a, sizeof a, "Connected"); snprintf(b, sizeof b, "%.60s", st); dot = OKC; }
    else { snprintf(a, sizeof a, paired ? "On, not connected" : "On, not paired"); snprintf(b, sizeof b, "%s", paired ? "Tap CONNECT, or connect from the other device" : "Tap PAIR, then pick \"PhoneKit\" on the other device"); dot = WARN; }
    int sh = P(64);
    cv_rrect(&cv, x, y, lw, sh, P(12), CARD, 255);
    cv_circle(&cv, x + P(20), y + sh / 2, P(6), dot, 255);
    cv_text(&cv, F_MEDIUM, P(14), x + P(36), y + sh / 2 - P(3), a, TEXT, 255, 0, lw - P(48));
    cv_text(&cv, F_REGULAR, P(10), x + P(36), y + sh / 2 + P(15), b, MUTED, 255, 0, lw - P(48));
    y += sh + P(8);
    int th = P(52);
    toggle_row(H_TOGGLE_ON, x, y, lw, th, "Bluetooth", NULL, store_get_int("bt", 0));
    y += th + P(8);
    toggle_row(H_TOGGLE_WIRED, x, y, lw, th, "Keep off while wired", "The USB keyboard works on the cable anyway", store_get_int("bt.wired_off", 1));
    y += th + P(8);
    if (land) { x = pad + lw + pad; y = top; }
    int bh = P(52);
    if (bt_wanted()) {
        button(H_PAIR, x, y, lw, bh, "PAIR A NEW DEVICE (3 MIN)", 0x1d3a5f, TEXT); y += bh + P(8);
        if (paired) { button(H_CONNECT, x, y, lw, bh, bthid_connected() ? "DISCONNECT" : "CONNECT TO THE PAIRED DEVICE", 0x1d4f4a, TEXT); y += bh + P(8); }
    }
    if (paired) { button(H_FORGET, x, y, lw, bh, "FORGET THE PAIRED DEVICE", 0x3a1d22, 0xfca5a5); y += bh + P(8); }
    if (msg[0] && now_ms() < msg_until) cv_text(&cv, F_REGULAR, P(11), x + P(4), y + P(14), msg, WARN, 255, 0, lw);
}

/* ---------------------------------------------------------------- glue */

void netui_draw(void) {
    if (!fb || screen == NET_NONE) return;
    if (cv_alloc(&cv, fb->w, fb->h) < 0) return;
    S = fminf(fb->w, fb->h) / 360.0f;
    nhit = 0;
    cv_clear(&cv, BG);
    if (screen == NET_BT) draw_bt();
    else if (pass_open) draw_pass();
    else draw_wifi();
    fb_blit(fb, &cv, 0, 0);
    fb_present(fb);
}

void netui_open(struct fb *f, int which) {
    fb = f;
    screen = which;
    pass_open = 0;
    msg[0] = 0;
    if (which == NET_WIFI && !nnet && !scanning) start_scan(); /* most people come here to pick a network */
    netui_draw();
}

int netui_active(void) { return screen != NET_NONE; }

void netui_close(void) {
    if (screen == NET_WIFI) wifi_radio_idle();
    screen = NET_NONE;
    pass_open = 0;
    memset(pw, 0, sizeof pw);
}

static void on_tap(int id) {
    if (id == H_BACK) { if (pass_open) { pass_open = 0; memset(pw, 0, sizeof pw); } else { netui_close(); hm_settings_redraw(); return; } }
    else if (screen == NET_WIFI && !pass_open) {
        if (id == H_TOGGLE_ON) store_set("wifi.on", store_get_int("wifi.on", 1) ? "0" : "1");
        else if (id == H_TOGGLE_WIRED) store_set("wifi.wired_off", store_get_int("wifi.wired_off", 1) ? "0" : "1");
        else if (id == H_SCAN) start_scan();
        else if (id == H_FORGET) { wifi_forget(); say("Network forgotten"); }
        else if (id >= H_NET0 && id < H_NET0 + nnet) {
            const struct net *n = &nets[id - H_NET0];
            snprintf(sel_ssid, sizeof sel_ssid, "%s", n->ssid);
            if (n->sae_only) say("WPA3-only networks aren't supported yet");
            else if (!n->secure) { const char *bad = wifi_join(sel_ssid, ""); say(bad ? bad : "Joining..."); }
            else { pass_open = 1; memset(pw, 0, sizeof pw); pw_show = 0; kb_shift = 0; kb_sym = 0; msg[0] = 0; }
        }
    } else if (screen == NET_WIFI && pass_open) {
        size_t n = strlen(pw);
        if (id == H_KEY0 + 1) kb_shift = !kb_shift;
        else if (id == H_KEY0 + 2) { if (n) pw[n - 1] = 0; }
        else if (id == H_KEY0 + 3) kb_sym = !kb_sym;
        else if (id >= H_KEY0 + 32 && id <= H_KEY0 + 126) { if (n < sizeof pw - 1) { pw[n] = (char)(id - H_KEY0); pw[n + 1] = 0; } kb_shift = 0; }
        else if (id == H_SHOW) pw_show = !pw_show;
        else if (id == H_CANCEL) { pass_open = 0; memset(pw, 0, sizeof pw); }
        else if (id == H_JOIN) {
            const char *bad = wifi_join(sel_ssid, pw);
            if (bad) say(bad);
            else { pass_open = 0; memset(pw, 0, sizeof pw); say(wifi_wanted() ? "Joining..." : "Saved: Wi-Fi comes on when the cable is out"); }
        }
    } else if (screen == NET_BT) {
        if (id == H_TOGGLE_ON) store_set("bt", store_get_int("bt", 0) ? "0" : "1");
        else if (id == H_TOGGLE_WIRED) store_set("bt.wired_off", store_get_int("bt.wired_off", 1) ? "0" : "1");
        else if (id == H_PAIR) { bthid_pair(); say("Visible as \"PhoneKit\" for 3 minutes: pair it from the other device"); }
        else if (id == H_CONNECT) { if (bthid_connected()) bthid_disconnect(); else bthid_connect(); say(bthid_connected() ? "Disconnecting..." : "Calling the paired device..."); }
        else if (id == H_FORGET) { bt_forget(); say("Forgotten. Pair again to use it"); }
    }
    netui_draw();
}

static int touch_id = -1, touch_hit;
void netui_touch(const int *ids, const int *xs, const int *ys, int n) {
    int still = 0;
    for (int i = 0; i < n; i++) if (ids[i] == touch_id) still = 1;
    if (touch_id >= 0 && !still) { /* lifted: act if it's still over the same thing */
        int h = touch_hit;
        touch_id = -1;
        if (h) on_tap(h);
        return;
    }
    if (touch_id >= 0 && still) { /* moved off: cancel */
        for (int i = 0; i < n; i++) if (ids[i] == touch_id && hit_at(xs[i], ys[i]) != touch_hit) touch_hit = 0;
        return;
    }
    if (n > 0) { touch_id = ids[0]; touch_hit = hit_at(xs[0], ys[0]); }
}

/* Once a second while showing: scan results, status changes, messages expiring. Returns 1 if it redrew. */
int netui_tick(void) {
    static long last;
    if (screen == NET_NONE || now_ms() - last < 1000) return 0;
    last = now_ms();
    if (scan_done) scan_done = 0;
    if (pass_open) return 0; /* don't fight the typing */
    netui_draw();
    return 1;
}

/* Side keys: vol up/down do nothing here yet; the mode key (hw.c) and power go back. */
int netui_key(int code) {
    if (screen == NET_NONE) return 0;
    if (hw_is_mode_key(code) || code == KEY_POWER) { on_tap(H_BACK); return 1; }
    return 0;
}
