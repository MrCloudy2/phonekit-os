// DECK mode (M6h): a stream-deck style trigger pad. Pages of 12 big buttons; each one sends
//   a key combination   "ctrl+shift+f13"   held while the finger is down (push-to-talk works)
//   a key sequence      "ctrl+c ctrl+v"    tapped in order on press
//   a media key         "media:play"       play next prev stop mute volup voldown (USB consumer control)
//   typed text          "text:gg wp"
// over the USB keyboard (mode A) and to a connected Bluetooth host (M10c). The defaults use F13-F24, which apps
// rarely bind otherwise: OBS, Discord, Streamlabs, ... take them as global hotkeys, so no host software is needed.
// Buttons can be held together; their keys are merged into one report.
//
//   ppctl deck                                         list pages and buttons
//   ppctl deck set N "LABEL" "ACTION" [colour] [toggle]  N = 1..48 (page 1 is 1-12, page 2 is 13-24, ...)
//   ppctl deck blank N | deck reset N | deck reset all
//   ppctl deck page P "NAME"                           page title (P = 1..4)
//   ppctl deck press N                                 press + release (tests)
//   ppctl deck style glossy|glass|flat                 button look (default glossy)
//   ppctl deck hints on|off                            the small key line under each label
// Labels: "\n" starts a new line ("BE RIGHT\nBACK"), up to 3 lines.
// Colours: blue red orange amber green teal violet pink gray, or #rrggbb. "toggle" buttons light up on every other
// press (a reminder of state such as a muted mic; PhoneKit can't see the app's real state).
#include "paneld.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PAGES 4
#define PER 12
#define NBTN (PAGES * PER)

struct btn { char label[24], action[100], colour[12]; int toggle; };
static const struct { const char *l, *a, *c; int t; } defaults[NBTN] = {
    /* page 1: streaming (bind these F-keys in OBS / Discord / ...) */
    {"SCENE 1", "f13", "blue", 0}, {"SCENE 2", "f14", "blue", 0}, {"SCENE 3", "f15", "blue", 0}, {"SCENE 4", "f16", "blue", 0},
    {"MIC", "f17", "red", 1}, {"CAMERA", "f18", "red", 1}, {"RECORD", "f19", "orange", 1}, {"LIVE", "f20", "orange", 1},
    {"BRB", "f21", "violet", 0}, {"CLIP", "f22", "teal", 0}, {"MARKER", "f23", "teal", 0}, {"F24", "f24", "gray", 0},
    /* page 2: media and desktop */
    {"PLAY / PAUSE", "media:play", "green", 0}, {"PREVIOUS", "media:prev", "green", 0}, {"NEXT", "media:next", "green", 0},
    {"MUTE", "media:mute", "amber", 0}, {"VOLUME -", "media:voldown", "amber", 0}, {"VOLUME +", "media:volup", "amber", 0},
    {"COPY", "ctrl+c", "gray", 0}, {"PASTE", "ctrl+v", "gray", 0}, {"UNDO", "ctrl+z", "gray", 0},
    {"SCREENSHOT", "prtsc", "teal", 0}, {"DESKTOP", "gui+d", "blue", 0}, {"LOCK", "gui+l", "red", 0},
};
static const char *const default_page_name[PAGES] = {"STREAM", "MEDIA", "PAGE 3", "PAGE 4"};

static struct btn B[NBTN];
static int toggled[NBTN];
static int loaded, page;
enum { ST_GLOSSY, ST_GLASS, ST_FLAT };
static const char *const style_name[] = {"glossy", "glass", "flat"};
static int style, hints = 1;

/* ---------------------------------------------------------------- config */

static void load_one(int i) {
    char k[16];
    snprintf(k, sizeof k, "deck.%d", i + 1);
    const char *v = store_get(k, NULL);
    struct btn *b = &B[i];
    memset(b, 0, sizeof *b);
    if (!v) { /* default */
        if (defaults[i].l) {
            snprintf(b->label, sizeof b->label, "%s", defaults[i].l);
            snprintf(b->action, sizeof b->action, "%s", defaults[i].a);
            snprintf(b->colour, sizeof b->colour, "%s", defaults[i].c);
            b->toggle = defaults[i].t;
        }
        return;
    }
    if (!strcmp(v, "-")) return; /* blanked */
    /* label|action|colour|t */
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", v);
    char *f[4] = {tmp, NULL, NULL, NULL};
    for (int j = 1; j < 4; j++) { char *p = f[j - 1] ? strchr(f[j - 1], '|') : NULL; if (p) { *p = 0; f[j] = p + 1; } }
    snprintf(b->label, sizeof b->label, "%s", f[0]);
    snprintf(b->action, sizeof b->action, "%s", f[1] ? f[1] : "");
    snprintf(b->colour, sizeof b->colour, "%s", f[2] && *f[2] ? f[2] : "gray");
    b->toggle = f[3] && *f[3] == 't';
}

static void load(void) {
    for (int i = 0; i < NBTN; i++) load_one(i);
    const char *st = store_get("deck.style", "glossy");
    style = ST_GLOSSY;
    for (int k = 0; k < 3; k++) if (!strcmp(st, style_name[k])) style = k;
    hints = strcmp(store_get("deck.hints", "on"), "off") != 0;
    loaded = 1;
}

static const char *page_name(int p) {
    char k[16];
    snprintf(k, sizeof k, "deck.page%d", p + 1);
    return store_get(k, default_page_name[p]);
}

static int page_used(int p) {
    if (p == 0) return 1;
    for (int i = p * PER; i < (p + 1) * PER; i++) if (B[i].label[0] || B[i].action[0]) return 1;
    return 0;
}

/* ---------------------------------------------------------------- actions */

static const struct { const char *n; int code; } KEYS[] = {
    {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29}, {"backspace", 0x2a}, {"bksp", 0x2a},
    {"tab", 0x2b}, {"space", 0x2c}, {"minus", 0x2d}, {"equal", 0x2e}, {"lbracket", 0x2f}, {"rbracket", 0x30},
    {"backslash", 0x31}, {"semicolon", 0x33}, {"quote", 0x34}, {"grave", 0x35}, {"comma", 0x36}, {"period", 0x37},
    {"slash", 0x38}, {"capslock", 0x39}, {"prtsc", 0x46}, {"printscreen", 0x46}, {"scrolllock", 0x47},
    {"pause", 0x48}, {"insert", 0x49}, {"ins", 0x49}, {"home", 0x4a}, {"pgup", 0x4b}, {"pageup", 0x4b},
    {"delete", 0x4c}, {"del", 0x4c}, {"end", 0x4d}, {"pgdn", 0x4e}, {"pagedown", 0x4e}, {"right", 0x4f},
    {"left", 0x50}, {"down", 0x51}, {"up", 0x52}, {"numlock", 0x53}, {"menu", 0x65}, {"app", 0x65},
    {"-", 0x2d}, {"=", 0x2e}, {"[", 0x2f}, {"]", 0x30}, {";", 0x33}, {"'", 0x34}, {"`", 0x35}, {",", 0x36},
    {".", 0x37}, {"/", 0x38},
};
static const struct { const char *n; int bit; } MODS[] = {
    {"ctrl", 0x01}, {"control", 0x01}, {"shift", 0x02}, {"alt", 0x04}, {"option", 0x04}, {"gui", 0x08},
    {"win", 0x08}, {"super", 0x08}, {"cmd", 0x08}, {"meta", 0x08}, {"rctrl", 0x10}, {"rshift", 0x20},
    {"ralt", 0x40}, {"altgr", 0x40}, {"rgui", 0x80},
};
static const struct { const char *n; int usage; } MEDIA[] = {
    {"play", 0xCD}, {"playpause", 0xCD}, {"next", 0xB5}, {"prev", 0xB6}, {"previous", 0xB6}, {"stop", 0xB7},
    {"mute", 0xE2}, {"volup", 0xE9}, {"voldown", 0xEA}, {"brightnessup", 0x6F}, {"brightnessdown", 0x70},
    {"calculator", 0x192}, {"browser", 0x196}, {"mail", 0x18A},
};

static int key_code(const char *t) {
    size_t l = strlen(t);
    if (l == 1 && t[0] >= 'a' && t[0] <= 'z') return 0x04 + t[0] - 'a';
    if (l == 1 && t[0] >= '1' && t[0] <= '9') return 0x1e + t[0] - '1';
    if (l == 1 && t[0] == '0') return 0x27;
    if ((t[0] == 'f') && l >= 2 && l <= 3) {
        int n = atoi(t + 1);
        if (n >= 1 && n <= 12) return 0x3a + n - 1;
        if (n >= 13 && n <= 24) return 0x68 + n - 13;
    }
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (!strcmp(t, KEYS[i].n)) return KEYS[i].code;
    return 0;
}

/* "ctrl+shift+f13" -> modifier bits + up to 6 keys. Returns 0, or -1 if a part isn't a key name. */
static int parse_combo(const char *s, size_t len, int *mods, unsigned char *keys, int *nk) {
    *mods = 0; *nk = 0;
    char t[100];
    if (len >= sizeof t) return -1;
    memcpy(t, s, len); t[len] = 0;
    for (char *p = t; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    char *save = NULL;
    for (char *part = strtok_r(t, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
        int m = 0;
        for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++) if (!strcmp(part, MODS[i].n)) m = MODS[i].bit;
        if (m) { *mods |= m; continue; }
        int c = key_code(part);
        if (!c) return -1;
        if (*nk < 6) keys[(*nk)++] = (unsigned char)c;
    }
    return 0;
}

static int media_usage(const char *a) {
    if (strncasecmp(a, "media:", 6)) return 0;
    for (size_t i = 0; i < sizeof MEDIA / sizeof MEDIA[0]; i++) if (!strcasecmp(a + 6, MEDIA[i].n)) return MEDIA[i].usage;
    return 0;
}

/* One combo (no spaces): held while pressed. Otherwise it's a sequence, text or media key. */
static int is_hold(const char *a) { return a[0] && !strchr(a, ' ') && strncasecmp(a, "text:", 5) && strncasecmp(a, "media:", 6); }

/* Validates an action: NULL if fine, else what's wrong. */
static const char *check_action(const char *a) {
    if (!a[0]) return NULL;
    if (!strncasecmp(a, "text:", 5)) return NULL;
    if (!strncasecmp(a, "media:", 6)) return media_usage(a) ? NULL : "unknown media key (play next prev stop mute volup voldown ...)";
    const char *p = a;
    while (*p) {
        while (*p == ' ') p++;
        size_t l = strcspn(p, " ");
        if (!l) break;
        int m, nk;
        unsigned char k[6];
        if (parse_combo(p, l, &m, k, &nk) < 0 || (!nk && !m)) return "unknown key name";
        p += l;
    }
    return NULL;
}

/* ---- held buttons: one finger each; their combos are merged into one keyboard report */
static struct { int id, btn; } held[10];
static int nheld;
static int media_down;

static void send_held(void) {
    int mods = 0, n = 0;
    unsigned char keys[6] = {0};
    for (int h = 0; h < nheld; h++) {
        const char *a = B[held[h].btn].action;
        if (!is_hold(a)) continue;
        int m, nk;
        unsigned char k[6];
        if (parse_combo(a, strlen(a), &m, k, &nk) < 0) continue;
        mods |= m;
        for (int j = 0; j < nk; j++) {
            int dup = 0;
            for (int x = 0; x < n; x++) dup |= keys[x] == k[j];
            if (!dup && n < 6) keys[n++] = k[j];
        }
    }
    hm_kbd_raw(mods, keys);
}

static void run_once(const char *a) { /* sequences and text: everything happens on press */
    if (!strncasecmp(a, "text:", 5)) { type_hid_text(a + 5); return; }
    const char *p = a;
    while (*p) {
        while (*p == ' ') p++;
        size_t l = strcspn(p, " ");
        if (!l) break;
        int m, nk;
        unsigned char k[6];
        if (parse_combo(p, l, &m, k, &nk) == 0) {
            hm_kbd_raw(m, k);
            usleep(15000);
            unsigned char z[6] = {0};
            hm_kbd_raw(0, z);
            usleep(15000);
        }
        p += l;
    }
}

static void press(int i) {
    const char *a = B[i].action;
    if (B[i].toggle) toggled[i] = !toggled[i];
    int u = media_usage(a);
    if (u) { hm_consumer(u); media_down = 1; return; }
    if (is_hold(a)) { send_held(); return; }
    if (a[0]) run_once(a);
}

static void release(int i) {
    if (media_usage(B[i].action) && media_down) { hm_consumer(0); media_down = 0; }
    if (is_hold(B[i].action)) send_held();
}

void deck_release_all(void) {
    int was = nheld;
    nheld = 0;
    if (was) { unsigned char z[6] = {0}; hm_kbd_raw(0, z); }
    if (media_down) { hm_consumer(0); media_down = 0; }
}

/* ---------------------------------------------------------------- drawing */

#define BG 0x06080b
#define TEXT 0xeef2f6
#define MUTED 0x8a94a3

static struct canvas cv, tmp;
static float S;
#define P(x) ((int)((x) * S + 0.5f))

static uint32_t colour_of(const char *c) {
    static const struct { const char *n; uint32_t v; } C[] = {
        {"blue", 0x60a5fa}, {"red", 0xef4444}, {"orange", 0xf97316}, {"amber", 0xf59e0b}, {"yellow", 0xfacc15},
        {"green", 0x22c55e}, {"lime", 0x84cc16}, {"teal", 0x2dd4bf}, {"cyan", 0x38bdf8}, {"violet", 0xa78bfa},
        {"purple", 0xa78bfa}, {"pink", 0xf472b6}, {"gray", 0x94a3b8}, {"grey", 0x94a3b8}, {"white", 0xeef2f6}};
    if (c[0] == '#' && strlen(c) == 7) return (uint32_t)strtoul(c + 1, NULL, 16);
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) if (!strcasecmp(c, C[i].n)) return C[i].v;
    return 0x94a3b8;
}

/* Near-square keys like a Stream Deck: 6 x 2 in landscape, 3 x 4 in portrait. */
static int cols(void) { return cv.w >= cv.h ? 6 : 3; }
static int header_h(void) { return P(46); }

static void btn_rect(int slot, int *x, int *y, int *w, int *h) {
    int c = cols(), r = PER / c, pad = P(10), gap = P(8), top = header_h();
    int aw = cv.w - 2 * pad, ah = cv.h - top - pad;
    int bw = (aw - (c - 1) * gap) / c, bh = (ah - (r - 1) * gap) / r;
    if (bh > bw * 23 / 20) bh = bw * 23 / 20; /* at most 15 % taller than wide */
    if (bw > bh * 23 / 20) bw = bh * 23 / 20;
    int ox = pad + (aw - (c * bw + (c - 1) * gap)) / 2, oy = top + (ah - (r * bh + (r - 1) * gap)) / 2;
    *x = ox + (slot % c) * (bw + gap);
    *y = oy + (slot / c) * (bh + gap);
    *w = bw; *h = bh;
}

/* How an action reads under the label: "CTRL+SHIFT+F13", "MEDIA PLAY", "TEXT". */
static void hint(const char *a, char *o, size_t ol) {
    if (!strncasecmp(a, "text:", 5)) { snprintf(o, ol, "TYPES \"%.20s\"", a + 5); return; }
    if (!strncasecmp(a, "media:", 6)) { snprintf(o, ol, "MEDIA %s", a + 6); }
    else snprintf(o, ol, "%s", a);
    for (char *p = o; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
}

/* Signed distance to a rounded rectangle (<0 inside). */
static float sd_rr(float px, float py, float x, float y, float w, float h, float r) {
    float hx = w * 0.5f, hy = h * 0.5f;
    float qx = fabsf(px - (x + hx)) - (hx - r), qy = fabsf(py - (y + hy)) - (hy - r);
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Room around a button for its shadow and glow (half the gap between buttons). */
static int margin(void) { return style == ST_FLAT ? 2 : P(4); }

/* A glossy (or glass) key in one pass per pixel: drop shadow, glow when lit, metallic rim, colour gradient with a
 * glow at the bottom, a curved gloss on the top half. Pressed keys sink by 2 dp and lose the shadow. */
__attribute__((optimize("O2"))) /* the hot loop of a key press; paneld is built -Os */
static void draw_key(int x, int y, int w, int h, int r, uint32_t c, int pressed, int lit, int dull) {
    int glass = style == ST_GLASS, M = margin(), dy = pressed ? P(2) : 0;
    float rim = fmaxf(2.0f, S * 1.6f), by = (float)(y + dy);
    uint32_t base = glass ? c : rgb_mix(c, 0x000000, 40); /* the palette is light; keys want deeper colour */
    if (dull) base = rgb_mix(base, 0x262b33, 120);
    if (pressed) base = rgb_mix(base, 0x000000, 60);
    uint32_t top = rgb_mix(base, 0xffffff, 40), bot = rgb_mix(base, 0x000000, 150), glow = rgb_mix(base, 0xffffff, 110);
    uint32_t tint = rgb_mix(0x0b0f15, base, lit || pressed ? 120 : dull ? 30 : 60);
    float cx = x + w * 0.5f, inv_ex = 1 / (w * 0.55f), inv_gx = 1 / (w * 0.75f);
    float gloss_max = (glass ? 70 : 125) * (pressed ? 0.45f : 1), g_in = P(2.5f);
    for (int j = y - M; j < y + h + M; j++) {
        if (j < 0 || j >= cv.h) continue;
        /* everything that depends only on the row */
        float py = j + 0.5f, t = clamp01((py - by) / h);
        float ey = (py - (by + h)) / (h * 0.5f), ey2 = ey * ey;
        float gy = (py - (by - h * 0.22f)) / (h * 0.68f), gy2 = gy * gy;
        float gv = gloss_max * (1 - 0.8f * clamp01((py - by) / (h * 0.46f)));
        uint32_t rc = glass ? rgb_mix(0xffffff, 0x8b95a5, (int)(200 * t)) : rgb_mix(0xeef1f5, 0x59606c, (int)(255 * t));
        uint32_t body0 = glass ? rgb_mix(tint, 0x000000, (int)(80 * t))
                               : t < 0.5f ? rgb_mix(top, base, (int)(510 * t)) : rgb_mix(base, bot, (int)(510 * (t - 0.5f)));
        int edge_a = t < 0.5f ? (int)(90 * (1 - 2 * t)) : 0;
        uint32_t *row = cv.px + (size_t)j * cv.w;
        for (int i = x - M; i < x + w + M; i++) {
            if (i < 0 || i >= cv.w) continue;
            float px = i + 0.5f;
            uint32_t col = BG;
            float d = sd_rr(px, py, (float)x, by, (float)w, (float)h, (float)r);
            if (d > -1) { /* outside or on the edge: shadow and glow */
                if (!pressed) {
                    float ds = sd_rr(px, py - P(3), (float)x, (float)y, (float)w, (float)h, (float)r);
                    float sa = clamp01(1 - (ds + P(1)) / P(4));
                    if (sa > 0) col = rgb_mix(col, 0x000000, (int)(170 * sa));
                }
                if (lit && d > 0) { float ga = clamp01(1 - d / M); col = rgb_mix(col, c, (int)(140 * ga * ga)); }
            }
            float co = clamp01(0.5f - d);
            if (co > 0) {
                col = co >= 1 ? rc : rgb_mix(col, rc, (int)(255 * co));
                float di = d + rim, ci = clamp01(0.5f - di);
                if (ci > 0) {
                    uint32_t body = body0;
                    float ex = (px - cx) * inv_ex, g = 1 - (ex * ex + ey2);
                    if (g > 0) body = rgb_mix(body, glass ? base : glow, (int)(g * g * (glass ? 110 : 160)));
                    col = ci >= 1 ? body : rgb_mix(col, body, (int)(255 * ci));
                    float gx = (px - cx) * inv_gx, e = gx * gx + gy2;
                    if (e < 1 && di < -g_in) { /* gloss: bright at the top, fading down to a crisp curved edge */
                        float ga = gv * clamp01((1 - e) * 30) * clamp01((-di - g_in) / 2);
                        col = rgb_mix(col, 0xffffff, (int)(ga * ci));
                    }
                    if (edge_a && di > -P(1.2f)) col = rgb_mix(col, 0xffffff, (int)(edge_a * ci)); /* inner top edge */
                }
            }
            row[i] = col;
        }
    }
}

/* Label lines: "\n" (backslash n) splits, up to 3. Returns the count. */
static int label_lines(const char *l, char out[3][24]) {
    int n = 0;
    const char *p = l;
    while (n < 3) {
        const char *q = strstr(p, "\\n");
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len > 23) len = 23;
        memcpy(out[n], p, len); out[n][len] = 0;
        n++;
        if (!q) break;
        p = q + 2;
    }
    return n;
}

static void text_shadowed(int font, int size, int x, int y, const char *s, uint32_t rgb, int a, float ls, int max_w) {
    if (style != ST_FLAT) cv_text(&cv, font, size, x, y + P(1), s, 0x000000, 110, ls, max_w);
    cv_text(&cv, font, size, x, y, s, rgb, a, ls, max_w);
}

static void draw_button(int slot, int pressed) {
    int i = page * PER + slot, x, y, w, h, M = margin();
    btn_rect(slot, &x, &y, &w, &h);
    cv_rect(&cv, x - M, y - M, w + 2 * M, h + 2 * M, BG, 255);
    const struct btn *b = &B[i];
    int r = P(style == ST_FLAT ? 16 : 18);
    if (!b->label[0] && !b->action[0]) { cv_rrect_stroke(&cv, x, y, w, h, r, fmaxf(1.5f, S * 0.6f), 0xffffff, 14); return; }
    uint32_t c = colour_of(b->colour);
    int on = b->toggle && toggled[i];
    if (style == ST_FLAT) {
        uint32_t fill = pressed ? rgb_mix(BG, c, 200) : on ? rgb_mix(BG, c, 150) : rgb_mix(BG, c, 34);
        cv_rrect(&cv, x, y, w, h, r, fill, 255);
        cv_rrect_stroke(&cv, x, y, w, h, r, fmaxf(2.0f, S * 0.8f), pressed ? 0xffffff : c, pressed ? 200 : on ? 255 : 110);
    } else {
        draw_key(x, y, w, h, r, c, pressed, on, b->toggle && !on);
        if (pressed) y += P(2);
    }
    if (b->toggle) { /* status light */
        float lx = x + w - P(17), ly = y + P(17);
        if (on && style != ST_FLAT) cv_circle(&cv, lx, ly, P(9), 0xffffff, 60);
        cv_circle(&cv, lx, ly, P(5), on ? 0xffffff : style == ST_FLAT ? c : 0x11151b, on ? 255 : style == ST_FLAT ? 120 : 160);
    }
    /* label: as large as fits, centred, one to three lines */
    char ln[3][24];
    int n = label_lines(b->label[0] ? b->label : b->action, ln);
    int hint_room = hints ? P(18) : 0;
    int ls = P(n == 1 ? 24 : 20);
    for (;; ls--) {
        int fits = ls <= P(10) || ls * 1.18f * n <= h - hint_room - P(16);
        for (int k = 0; k < n && fits && ls > P(10); k++) fits = cv_text_w(F_BOLD, ls, ln[k], ls * 0.06f) <= w - P(20);
        if (fits) break;
    }
    float lh = ls * 1.18f;
    float cy = y + (h - hint_room * 0.6f) / 2 - lh * (n - 1) / 2;
    uint32_t tc = style == ST_FLAT ? (pressed || on ? 0xffffff : TEXT) : 0xffffff;
    for (int k = 0; k < n; k++) {
        int tw = cv_text_w(F_BOLD, ls, ln[k], ls * 0.06f);
        text_shadowed(F_BOLD, ls, x + (w - tw) / 2, (int)(cy + k * lh + ls * 0.36f), ln[k], tc, 255, ls * 0.06f, w - P(12));
    }
    if (!hints) return;
    char hs[48];
    hint(b->action, hs, sizeof hs);
    int hsz = P(10);
    int hw = cv_text_w(F_MEDIUM, hsz, hs, hsz * 0.10f);
    if (hw > w - P(16)) hw = w - P(16);
    int ha = style == ST_FLAT ? (pressed || on ? 210 : 255) : 200;
    uint32_t hc = style == ST_FLAT ? (pressed || on ? 0xffffff : MUTED) : 0xffffff;
    text_shadowed(F_MEDIUM, hsz, x + (w - hw) / 2, y + h - P(14), hs, hc, ha, hsz * 0.10f, w - P(16));
}

static int usb_ok(void) { return gadget_mode() == 'A' && !strncmp(gadget_state(), "configured", 10); }

/* tab rectangles in the header, for drawing and hit-testing */
static int tab_x[PAGES], tab_w[PAGES];

static void draw_header(void) {
    int hh = header_h();
    cv_rect(&cv, 0, 0, cv.w, hh, BG, 255);
    int ts = P(12), base = hh / 2 + (int)(ts * 0.38f) + P(2);
    int x = P(14);
    x = cv_text(&cv, F_BOLD, ts, x, base, "DECK", MUTED, 255, ts * 0.16f, 0) + P(16);
    for (int p = 0; p < PAGES; p++) {
        tab_w[p] = 0;
        if (!page_used(p)) continue;
        const char *n = page_name(p);
        int w = cv_text_w(F_MEDIUM, ts, n, ts * 0.10f) + P(24);
        tab_x[p] = x; tab_w[p] = w;
        if (p == page) cv_rrect(&cv, x, hh / 2 - P(13) + P(2), w, P(26), P(13), 0xffffff, 28);
        cv_text(&cv, F_MEDIUM, ts, x + P(12), base, n, p == page ? TEXT : MUTED, 255, ts * 0.10f, 0);
        x += w + P(6);
    }
    /* outputs: USB keyboard, Bluetooth host */
    const char *lab[2] = {"BT", "USB"};
    int ok[2] = {bthid_connected(), usb_ok()};
    int rx = cv.w - P(14);
    for (int k = 0; k < 2; k++) {
        int w = cv_text_w(F_MEDIUM, ts, lab[k], 0);
        rx -= w;
        cv_text(&cv, F_MEDIUM, ts, rx, base, lab[k], ok[k] ? TEXT : MUTED, 255, 0, 0);
        rx -= P(10);
        cv_circle(&cv, rx, base - ts * 0.36f, P(3.5f), ok[k] ? 0x22c55e : 0x5b6472, 255);
        rx -= P(18);
    }
}

static int shown_status; /* bt/usb state the header shows */
static int status_now(void) { return bthid_connected() | usb_ok() << 1; }

static struct fb *last_fb;

void deck_draw(struct fb *f) {
    if (!loaded) load();
    last_fb = f;
    if (cv_alloc(&cv, f->w, f->h) < 0) return;
    S = fminf(f->w, f->h) / 360.0f;
    if (!page_used(page)) page = 0;
    cv_clear(&cv, BG);
    draw_header();
    for (int s = 0; s < PER; s++) draw_button(s, 0);
    shown_status = status_now();
    fb_blit(f, &cv, 0, 0);
    fb_present(f);
}

static void blit_part(struct fb *f, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cv.w) w = cv.w - x;
    if (y + h > cv.h) h = cv.h - y;
    if (w <= 0 || h <= 0 || cv_alloc(&tmp, w, h) < 0) return;
    for (int j = 0; j < h; j++) memcpy(tmp.px + (size_t)j * w, cv.px + (size_t)(y + j) * cv.w + x, (size_t)w * 4);
    fb_blit(f, &tmp, x, y);
}

static void redraw_button(struct fb *f, int slot, int pressed) {
    draw_button(slot, pressed);
    int x, y, w, h, M = margin();
    btn_rect(slot, &x, &y, &w, &h);
    blit_part(f, x - M, y - M, w + 2 * M, h + 2 * M);
    fb_present(f);
}

/* Main loop (via hm_tick while DECK shows): refresh the BT/USB dots when they change. */
int deck_tick(struct fb *f) {
    if (!cv.w || f != last_fb) return 1000;
    if (status_now() != shown_status) {
        shown_status = status_now();
        draw_header();
        blit_part(f, 0, 0, cv.w, header_h());
        fb_present(f);
    }
    return 1000;
}

/* ---------------------------------------------------------------- touch */

static int slot_at(int x, int y) {
    for (int s = 0; s < PER; s++) {
        int bx, by, bw, bh;
        btn_rect(s, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) return s;
    }
    return -1;
}

int deck_touch(struct fb *f, const int *ids, const int *xs, const int *ys, int n) {
    if (!loaded) load();
    if (cv.w != f->w || cv.h != f->h) { deck_draw(f); }
    /* lifted fingers: release their buttons */
    for (int h = 0; h < nheld;) {
        int still = 0;
        for (int i = 0; i < n; i++) still |= ids[i] == held[h].id;
        if (still) { h++; continue; }
        int b = held[h].btn;
        held[h] = held[--nheld];
        release(b);
        if (b / PER == page) redraw_button(f, b % PER, 0);
    }
    /* new fingers */
    for (int i = 0; i < n; i++) {
        int known = 0;
        for (int h = 0; h < nheld; h++) known |= held[h].id == ids[i];
        if (known) continue;
        if (ys[i] < header_h()) { /* a page tab */
            static int tab_id = -1;
            if (tab_id == ids[i]) continue;
            tab_id = ids[i];
            for (int p = 0; p < PAGES; p++)
                if (tab_w[p] && xs[i] >= tab_x[p] && xs[i] < tab_x[p] + tab_w[p] && p != page && !nheld) {
                    page = p;
                    deck_draw(f);
                }
            continue;
        }
        int s = slot_at(xs[i], ys[i]);
        if (s < 0 || nheld >= 10) continue;
        int b = page * PER + s, dup = 0;
        for (int h = 0; h < nheld; h++) dup |= held[h].btn == b;
        if (dup || (!B[b].label[0] && !B[b].action[0])) continue;
        held[nheld].id = ids[i]; held[nheld].btn = b; nheld++;
        press(b); /* the key goes out first, the picture after */
        redraw_button(f, s, 1);
    }
    return 1;
}

/* ---------------------------------------------------------------- ctl */

static void save(int i) {
    char k[16], v[208];
    snprintf(k, sizeof k, "deck.%d", i + 1);
    snprintf(v, sizeof v, "%s|%s|%s|%s", B[i].label, B[i].action, B[i].colour, B[i].toggle ? "t" : "");
    store_set(k, v);
}

static void changed(void) {
    if (last_fb && hm_owner() == OWN_INPUT && hm_input_mode() == IN_DECK) deck_draw(last_fb);
}

int deck_ctl(char **argv, int argc, const char *raw, char *out, size_t len) {
    (void)raw;
    if (!loaded) load();
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "set") && argc >= 5) {
        int i = atoi(argv[2]) - 1;
        if (i < 0 || i >= NBTN) return snprintf(out, len, "error: button 1..%d\n", NBTN);
        if (strchr(argv[3], '|') || strchr(argv[4], '|')) return snprintf(out, len, "error: '|' not allowed\n");
        const char *bad = check_action(argv[4]);
        if (bad) return snprintf(out, len, "error: %s: %s\n", argv[4], bad);
        struct btn *b = &B[i];
        snprintf(b->label, sizeof b->label, "%s", argv[3]);
        snprintf(b->action, sizeof b->action, "%s", argv[4]);
        snprintf(b->colour, sizeof b->colour, "%s", argc > 5 && strcmp(argv[5], "toggle") ? argv[5] : "gray");
        b->toggle = (argc > 5 && !strcmp(argv[5], "toggle")) || (argc > 6 && !strcmp(argv[6], "toggle"));
        toggled[i] = 0;
        save(i);
        changed();
        return snprintf(out, len, "button %d: %s -> %s\n", i + 1, b->label, b->action);
    }
    if (!strcmp(c, "blank") && argc >= 3) {
        int i = atoi(argv[2]) - 1;
        if (i < 0 || i >= NBTN) return snprintf(out, len, "error: button 1..%d\n", NBTN);
        char k[16];
        snprintf(k, sizeof k, "deck.%d", i + 1);
        store_set(k, "-");
        load_one(i);
        changed();
        return snprintf(out, len, "button %d blank\n", i + 1);
    }
    if (!strcmp(c, "reset") && argc >= 3) {
        int all = !strcmp(argv[2], "all");
        int i0 = all ? 0 : atoi(argv[2]) - 1, i1 = all ? NBTN : i0 + 1;
        if (i0 < 0 || i0 >= NBTN) return snprintf(out, len, "error: button 1..%d or all\n", NBTN);
        for (int i = i0; i < i1; i++) {
            char k[16];
            snprintf(k, sizeof k, "deck.%d", i + 1);
            if (store_get(k, NULL)) store_unset(k);
            load_one(i);
            toggled[i] = 0;
        }
        if (all) for (int p = 0; p < PAGES; p++) { char k[16]; snprintf(k, sizeof k, "deck.page%d", p + 1); if (store_get(k, NULL)) store_unset(k); }
        changed();
        return snprintf(out, len, "%s back to the default\n", all ? "all buttons" : "button");
    }
    if (!strcmp(c, "page") && argc >= 4) {
        int p = atoi(argv[2]) - 1;
        if (p < 0 || p >= PAGES) return snprintf(out, len, "error: page 1..%d\n", PAGES);
        char k[16];
        snprintf(k, sizeof k, "deck.page%d", p + 1);
        store_set(k, argv[3]);
        changed();
        return snprintf(out, len, "page %d: %s\n", p + 1, argv[3]);
    }
    if (!strcmp(c, "press") && argc >= 3) {
        int i = atoi(argv[2]) - 1;
        if (i < 0 || i >= NBTN || (!B[i].label[0] && !B[i].action[0])) return snprintf(out, len, "error: no such button\n");
        if (nheld >= 10) return snprintf(out, len, "error: busy\n");
        held[nheld].id = -100; held[nheld].btn = i; nheld++;
        press(i);
        usleep(40000);
        for (int h = 0; h < nheld; h++) if (held[h].id == -100) { held[h] = held[--nheld]; break; }
        release(i);
        changed();
        return snprintf(out, len, "pressed %d (%s)\n", i + 1, B[i].action);
    }
    if (!strcmp(c, "style") && argc >= 3) {
        int k = -1;
        for (int j = 0; j < 3; j++) if (!strcmp(argv[2], style_name[j])) k = j;
        if (k < 0) return snprintf(out, len, "error: style glossy|glass|flat\n");
        style = k;
        store_set("deck.style", style_name[k]);
        changed();
        return snprintf(out, len, "style %s\n", style_name[k]);
    }
    if (!strcmp(c, "hints") && argc >= 3) {
        if (strcmp(argv[2], "on") && strcmp(argv[2], "off")) return snprintf(out, len, "error: hints on|off\n");
        hints = !strcmp(argv[2], "on");
        store_set("deck.hints", argv[2]);
        changed();
        return snprintf(out, len, "hints %s\n", argv[2]);
    }
    if (c[0]) return snprintf(out, len, "error: deck [set N LABEL ACTION [colour] [toggle] | blank N | reset N|all | page P NAME | press N | style glossy|glass|flat | hints on|off]\n");
    size_t o = 0;
    for (int p = 0; p < PAGES && o + 200 < len; p++) {
        if (!page_used(p)) continue;
        o += (size_t)snprintf(out + o, len - o, "page %d: %s%s\n", p + 1, page_name(p), p == page ? " (shown)" : "");
        for (int i = p * PER; i < (p + 1) * PER && o + 160 < len; i++) {
            const struct btn *b = &B[i];
            if (!b->label[0] && !b->action[0]) { o += (size_t)snprintf(out + o, len - o, "  %2d  -\n", i + 1); continue; }
            o += (size_t)snprintf(out + o, len - o, "  %2d  %-14s %-22s %s%s%s\n", i + 1, b->label, b->action, b->colour,
                                  b->toggle ? " toggle" : "", b->toggle && toggled[i] ? " (on)" : "");
        }
    }
    o += (size_t)snprintf(out + o, len - o, "style %s, hints %s\n", style_name[style], hints ? "on" : "off");
    o += (size_t)snprintf(out + o, len - o, "outputs: usb %s, bluetooth %s\n", usb_ok() ? "yes" : "no", bthid_connected() ? "yes" : "no");
    return (int)o;
}
