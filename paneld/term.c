// RESCUE mode: a terminal for a headless server's serial console.
//
// The server sees PhoneKit's third USB serial port (CDC-ACM, a driver every Linux has) and runs a login on it
// (`systemctl enable --now serial-getty@ttyACM2`, see docs). In RESCUE mode that port is the console instead of
// PhoneKit's own debug shell: bytes from the server go through a VT100/xterm-subset emulator drawn with Roboto Mono,
// and the on-screen keyboard types into it. No GUD, no network, no driver needed on the server.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAXC 200
#define MAXR 90
#define DEF_FG 0xd0d6de
#define DEF_BG 0x06080b
enum { A_BOLD = 1, A_UL = 2, A_REV = 4, A_DIM = 8 };
struct cell { uint32_t ch, fg, bg; uint8_t at; };

static struct cell scr[2][MAXR][MAXC];
static int alt;                       /* alternate screen in use */
static int cols = 80, rows = 24;
static int cx, cy, scx, scy, top, bot, wrapnext, autowrap = 1, cursor_on = 1, app_cursor;
static uint32_t cfg_fg = DEF_FG, cbg = DEF_BG;
static uint8_t cat;
static int acs[2], gl;                /* G0/G1 DEC line drawing, which one is in GL */
static uint8_t dirty[MAXR];
static int fd = -1;
static char tty[64];
static unsigned long bytes_in, bytes_out;

static uint32_t lastch = ' ';
static void put(uint32_t c);

/* parser */
enum { S_NORM, S_ESC, S_CSI, S_OSC, S_CHARSET, S_OSC_ESC };
static int st_, priv, np, par[16], charset_g;
static uint32_t u8acc;
static int u8need;

static struct fb *fb;
static struct canvas cv;
static int active;                    /* RESCUE is on screen */
static int remote;                    /* an ssh pk-console session has the console */
static int kb_shown = 1;
static int fsz = 30, cw = 18, ch_ = 36; /* font px, cell w/h */
static int tx0, ty0, kb_y;            /* terminal origin, keyboard top (logical px) */

/* ---------------------------------------------------------------- screen ops */

static struct cell blank(void) { return (struct cell){' ', cfg_fg, cbg, 0}; }
static struct cell (*S(void))[MAXC] { return scr[alt]; }
static void mark(int r) { if (r >= 0 && r < MAXR) dirty[r] = 1; }
static void mark_all(void) { memset(dirty, 1, sizeof dirty); }

static void clear_cells(int r, int c0, int c1) {
    struct cell b = blank();
    if (r < 0 || r >= rows) return;
    for (int c = c0 < 0 ? 0 : c0; c < c1 && c < cols; c++) S()[r][c] = b;
    mark(r);
}

static void scroll_up(int t, int b, int n) {
    for (int k = 0; k < n; k++) {
        memmove(&S()[t], &S()[t + 1], sizeof(S()[0]) * (size_t)(b - t));
        clear_cells(b, 0, cols);
    }
    for (int r = t; r <= b; r++) mark(r);
}

static void scroll_down(int t, int b, int n) {
    for (int k = 0; k < n; k++) {
        memmove(&S()[t + 1], &S()[t], sizeof(S()[0]) * (size_t)(b - t));
        clear_cells(t, 0, cols);
    }
    for (int r = t; r <= b; r++) mark(r);
}

static void newline(void) {
    if (cy == bot) scroll_up(top, bot, 1);
    else if (cy < rows - 1) cy++;
}

static void reset(void) {
    alt = 0; cx = cy = 0; top = 0; bot = rows - 1; wrapnext = 0; autowrap = 1; cursor_on = 1; app_cursor = 0;
    cfg_fg = DEF_FG; cbg = DEF_BG; cat = 0; acs[0] = acs[1] = 0; gl = 0;
    for (int a = 0; a < 2; a++) { alt = a; for (int r = 0; r < MAXR; r++) clear_cells(r, 0, MAXC); }
    alt = 0;
    mark_all();
}

static uint32_t pal(int i) {
    static const uint32_t base[16] = {0x1c1f26, 0xef4444, 0x22c55e, 0xeab308, 0x3b82f6, 0xa855f7, 0x06b6d4, 0xd0d6de,
                                      0x5b6472, 0xf87171, 0x4ade80, 0xfacc15, 0x60a5fa, 0xc084fc, 0x22d3ee, 0xf8fafc};
    if (i < 16) return base[i];
    if (i < 232) { i -= 16; int r = i / 36, g = i / 6 % 6, b = i % 6; return (uint32_t)((r ? r * 40 + 55 : 0) << 16 | (g ? g * 40 + 55 : 0) << 8 | (b ? b * 40 + 55 : 0)); }
    int v = 8 + (i - 232) * 10;
    return (uint32_t)(v << 16 | v << 8 | v);
}

static void sgr(void) {
    if (!np) { np = 1; par[0] = 0; }
    for (int i = 0; i < np; i++) {
        int p = par[i];
        if (p == 0) { cfg_fg = DEF_FG; cbg = DEF_BG; cat = 0; }
        else if (p == 1) cat |= A_BOLD;
        else if (p == 2) cat |= A_DIM;
        else if (p == 4) cat |= A_UL;
        else if (p == 7) cat |= A_REV;
        else if (p == 22) cat &= ~(A_BOLD | A_DIM);
        else if (p == 24) cat &= ~A_UL;
        else if (p == 27) cat &= ~A_REV;
        else if (p >= 30 && p <= 37) cfg_fg = pal(p - 30);
        else if (p == 39) cfg_fg = DEF_FG;
        else if (p >= 40 && p <= 47) cbg = pal(p - 40);
        else if (p == 49) cbg = DEF_BG;
        else if (p >= 90 && p <= 97) cfg_fg = pal(p - 90 + 8);
        else if (p >= 100 && p <= 107) cbg = pal(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 2 < np && par[i + 1] == 5) { uint32_t c = pal(par[i + 2] & 255); if (p == 38) cfg_fg = c; else cbg = c; i += 2; }
        else if ((p == 38 || p == 48) && i + 4 < np && par[i + 1] == 2) {
            uint32_t c = (uint32_t)((par[i + 2] & 255) << 16 | (par[i + 3] & 255) << 8 | (par[i + 4] & 255));
            if (p == 38) cfg_fg = c; else cbg = c;
            i += 4;
        }
    }
}

static void con_open(void) {
    static long last;
    if (fd >= 0 && now_ms() - last < 500) return; /* a dead port must not make us spin */
    last = now_ms();
    if (fd >= 0) close(fd);
    fd = tty[0] ? open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC) : -1;
    if (fd >= 0) {
        struct termios t;
        if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
    }
}

/* A USB re-bind hangs the gadget tty up (EIO / POLLHUP): reopen it, as the control channel does. */
static void tty_write(const char *s, size_t n) {
    for (int attempt = 0; attempt < 2 && fd >= 0; attempt++) {
        ssize_t w = write(fd, s, n);
        if (w > 0) { bytes_out += (unsigned long)w; return; }
        if (w < 0 && errno == EAGAIN) return;
        con_open();
    }
}

static void csi(int fin) {
    int p0 = np > 0 ? par[0] : 0, p1 = np > 1 ? par[1] : 0;
    int n = p0 ? p0 : 1;
    switch (fin) {
    case 'A': cy -= n; if (cy < top && cy + n >= top) cy = top; if (cy < 0) cy = 0; break;
    case 'B': case 'e': cy += n; if (cy > bot && cy - n <= bot) cy = bot; if (cy >= rows) cy = rows - 1; break;
    case 'C': case 'a': cx += n; if (cx >= cols) cx = cols - 1; break;
    case 'D': cx -= n; if (cx < 0) cx = 0; break;
    case 'E': cy += n; cx = 0; if (cy >= rows) cy = rows - 1; break;
    case 'F': cy -= n; cx = 0; if (cy < 0) cy = 0; break;
    case 'G': case '`': cx = n - 1; if (cx >= cols) cx = cols - 1; break;
    case 'd': cy = n - 1; if (cy >= rows) cy = rows - 1; break;
    case 'H': case 'f': cy = (p0 ? p0 : 1) - 1; cx = (p1 ? p1 : 1) - 1;
        if (cy >= rows) cy = rows - 1;
        if (cx >= cols) cx = cols - 1;
        break;
    case 'J':
        if (p0 == 0) { clear_cells(cy, cx, cols); for (int r = cy + 1; r < rows; r++) clear_cells(r, 0, cols); }
        else if (p0 == 1) { clear_cells(cy, 0, cx + 1); for (int r = 0; r < cy; r++) clear_cells(r, 0, cols); }
        else for (int r = 0; r < rows; r++) clear_cells(r, 0, cols);
        break;
    case 'K':
        if (p0 == 0) clear_cells(cy, cx, cols);
        else if (p0 == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, 0, cols);
        break;
    case 'L': if (cy >= top && cy <= bot) scroll_down(cy, bot, n > bot - cy + 1 ? bot - cy + 1 : n); break;
    case 'M': if (cy >= top && cy <= bot) scroll_up(cy, bot, n > bot - cy + 1 ? bot - cy + 1 : n); break;
    case 'S': scroll_up(top, bot, n); break;
    case 'T': scroll_down(top, bot, n); break;
    case 'P': {
        if (n > cols - cx) n = cols - cx;
        memmove(&S()[cy][cx], &S()[cy][cx + n], sizeof(struct cell) * (size_t)(cols - cx - n));
        clear_cells(cy, cols - n, cols);
        break;
    }
    case '@': {
        if (n > cols - cx) n = cols - cx;
        memmove(&S()[cy][cx + n], &S()[cy][cx], sizeof(struct cell) * (size_t)(cols - cx - n));
        clear_cells(cy, cx, cx + n);
        break;
    }
    case 'X': clear_cells(cy, cx, cx + n); break;
    case 'b': for (int i = 0; i < n && i < 1000; i++) put(lastch); break; /* REP: ncurses uses it (xterm "rep") */
    case 'r': top = (p0 ? p0 : 1) - 1; bot = (p1 ? p1 : rows) - 1;
        if (top < 0) top = 0;
        if (bot >= rows) bot = rows - 1;
        if (top >= bot) { top = 0; bot = rows - 1; }
        cx = cy = 0;
        break;
    case 's': scx = cx; scy = cy; break;
    case 'u': cx = scx; cy = scy; break;
    case 'm': sgr(); break;
    case 'n': if (p0 == 6) { char b[32]; int l = snprintf(b, sizeof b, "\x1b[%d;%dR", cy + 1, cx + 1); tty_write(b, (size_t)l); }
              else if (p0 == 5) tty_write("\x1b[0n", 4);
              break;
    case 'c': if (!priv) tty_write("\x1b[?1;2c", 7); break;
    case 'h': case 'l': {
        int on = fin == 'h';
        for (int i = 0; i < np; i++) {
            if (priv == '?') {
                if (par[i] == 25) cursor_on = on;
                else if (par[i] == 7) autowrap = on;
                else if (par[i] == 1) app_cursor = on;
                else if (par[i] == 1049 || par[i] == 1047 || par[i] == 47) {
                    if (on && !alt) { if (par[i] == 1049) { scx = cx; scy = cy; } alt = 1; for (int r = 0; r < rows; r++) clear_cells(r, 0, cols); }
                    else if (!on && alt) { alt = 0; if (par[i] == 1049) { cx = scx; cy = scy; } }
                    mark_all();
                }
            }
        }
        break;
    }
    default: break;
    }
    wrapnext = 0;
}

static uint32_t acs_map(uint32_t c) {
    static const char from[] = "jklmnqtuvwxa`~";
    static const uint32_t to[] = {0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x2500, 0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2592, 0x25c6, 0x00b7};
    const char *p = c < 128 ? strchr(from, (int)c) : NULL;
    return p && c ? to[p - from] : c;
}

static void put(uint32_t c) {
    lastch = c;
    if (acs[gl]) c = acs_map(c);
    if (wrapnext && autowrap) { cx = 0; newline(); }
    wrapnext = 0;
    S()[cy][cx] = (struct cell){c, cfg_fg, cbg, cat};
    mark(cy);
    if (cx == cols - 1) wrapnext = 1;
    else cx++;
}

static void feed(unsigned char b) {
    if (st_ == S_NORM && u8need) {
        if ((b & 0xc0) == 0x80) { u8acc = u8acc << 6 | (b & 63); if (--u8need == 0) put(u8acc); return; }
        u8need = 0;
    }
    switch (st_) {
    case S_NORM:
        if (b == 0x1b) { st_ = S_ESC; return; }
        if (b == '\r') { cx = 0; wrapnext = 0; return; }
        if (b == '\n' || b == 0x0b || b == 0x0c) { newline(); wrapnext = 0; return; }
        if (b == '\b') { if (cx > 0) cx--; wrapnext = 0; return; }
        if (b == '\t') { cx = (cx / 8 + 1) * 8; if (cx >= cols) cx = cols - 1; return; }
        if (b == 0x0e) { gl = 1; return; }
        if (b == 0x0f) { gl = 0; return; }
        if (b < 0x20 || b == 0x7f) return;
        if (b >= 0xc0) { u8need = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1; u8acc = b & (b >= 0xf0 ? 7 : b >= 0xe0 ? 15 : 31); return; }
        put(b);
        return;
    case S_ESC:
        st_ = S_NORM;
        if (b == '[') { st_ = S_CSI; np = 0; priv = 0; memset(par, 0, sizeof par); }
        else if (b == ']') st_ = S_OSC;
        else if (b == '(' || b == ')') { st_ = S_CHARSET; charset_g = b == ')'; }
        else if (b == '7') { scx = cx; scy = cy; }
        else if (b == '8') { cx = scx; cy = scy; }
        else if (b == 'D') newline();
        else if (b == 'E') { cx = 0; newline(); }
        else if (b == 'M') { if (cy == top) scroll_down(top, bot, 1); else if (cy > 0) cy--; }
        else if (b == 'c') reset();
        return;
    case S_CHARSET: acs[charset_g] = b == '0'; st_ = S_NORM; return;
    case S_CSI:
        if (b >= '0' && b <= '9') { if (!np) np = 1; par[np - 1] = par[np - 1] * 10 + (b - '0'); if (par[np - 1] > 9999) par[np - 1] = 9999; return; }
        if (b == ';' || b == ':') { if (!np) np = 1; if (np < 16) np++; return; }
        if (b == '?' || b == '>' || b == '=' || b == '!') { priv = b; return; }
        if (b >= 0x40 && b <= 0x7e) { st_ = S_NORM; csi(b); }
        return;
    case S_OSC: if (b == 7) st_ = S_NORM; else if (b == 0x1b) st_ = S_OSC_ESC; return;
    case S_OSC_ESC: st_ = S_NORM; return;
    }
}

/* ---------------------------------------------------------------- keyboard */

struct key { const char *lab, *out, *shift, *sym; float w; };
#define K(l, o, s, y) {l, o, s, y, 1}
static const struct key KB[6][12] = {
    {{"Esc", "\x1b", "\x1b", "\x1b", 1}, {"Tab", "\t", "\x1b[Z", "\t", 1}, {"Ctrl", NULL, NULL, NULL, 1}, {"Alt", NULL, NULL, NULL, 1},
     {"Left", "D", "D", "D", 1}, {"Down", "B", "B", "B", 1}, {"Up", "A", "A", "A", 1}, {"Right", "C", "C", "C", 1},
     {"PgUp", "\x1b[5~", "\x1b[5~", "\x1b[5~", 1}, {"PgDn", "\x1b[6~", "\x1b[6~", "\x1b[6~", 1}, {"Size", NULL, NULL, NULL, 1}, {"Hide", NULL, NULL, NULL, 1}},
    {K("1", "1", "!", "!"), K("2", "2", "@", "@"), K("3", "3", "#", "#"), K("4", "4", "$", "$"), K("5", "5", "%", "%"), K("6", "6", "^", "^"),
     K("7", "7", "&", "&"), K("8", "8", "*", "*"), K("9", "9", "(", "("), K("0", "0", ")", ")"), K("-", "-", "_", "_"), {"Bksp", "\x7f", "\x7f", "\x7f", 1}},
    {K("q", "q", "Q", "`"), K("w", "w", "W", "~"), K("e", "e", "E", "{"), K("r", "r", "R", "}"), K("t", "t", "T", "["), K("y", "y", "Y", "]"),
     K("u", "u", "U", "|"), K("i", "i", "I", "\\"), K("o", "o", "O", "+"), K("p", "p", "P", "="), K("/", "/", "?", "?"), K("=", "=", "+", "+")},
    {K("a", "a", "A", "<"), K("s", "s", "S", ">"), K("d", "d", "D", "\""), K("f", "f", "F", "'"), K("g", "g", "G", ":"), K("h", "h", "H", ";"),
     K("j", "j", "J", "~"), K("k", "k", "K", "`"), K("l", "l", "L", "|"), K(";", ";", ":", ":"), K("'", "'", "\"", "\""), {"Enter", "\r", "\r", "\r", 1}},
    {{"Shift", NULL, NULL, NULL, 1.5f}, K("z", "z", "Z", "("), K("x", "x", "X", ")"), K("c", "c", "C", "{"), K("v", "v", "V", "}"), K("b", "b", "B", "["),
     K("n", "n", "N", "]"), K("m", "m", "M", "<"), K(",", ",", "<", "<"), K(".", ".", ">", ">"), {"Sym", NULL, NULL, NULL, 1.5f}},
    {{"Home", "\x1b[H", "\x1b[H", "\x1b[H", 1.5f}, {"End", "\x1b[F", "\x1b[F", "\x1b[F", 1.5f}, {" ", " ", " ", " ", 5}, {"|", "|", "|", "|", 1}, {"\\", "\\", "\\", "\\", 1}, {"Out", NULL, NULL, NULL, 2}},
};
static const int KB_N[6] = {12, 12, 12, 12, 11, 6};
static int shift, ctrl, altk, sym;
static int hid_out; /* keys go to the server's USB keyboard (blind typing, e.g. to start a serial login) */

/* ASCII -> USB HID usage (US layout) + whether Shift is needed. 0 = no key. */
static int ascii_hid(unsigned char c, int *shift_needed) {
    static const char sh_digits[] = ")!@#$%^&*(";
    *shift_needed = 0;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') { *shift_needed = 1; return 0x04 + (c - 'A'); }
    if (c >= '1' && c <= '9') return 0x1e + (c - '1');
    if (c == '0') return 0x27;
    const char *d = strchr(sh_digits, c);
    if (d && c) { *shift_needed = 1; return d == sh_digits ? 0x27 : 0x1e + (int)(d - sh_digits) - 1; }
    static const struct { char plain, shifted; int code; } P[] = {
        {'-', '_', 0x2d}, {'=', '+', 0x2e}, {'[', '{', 0x2f}, {']', '}', 0x30}, {'\\', '|', 0x31},
        {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35}, {',', '<', 0x36}, {'.', '>', 0x37}, {'/', '?', 0x38}};
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        if (c == (unsigned char)P[i].plain) return P[i].code;
        if (c == (unsigned char)P[i].shifted) { *shift_needed = 1; return P[i].code; }
    }
    if (c == '\r' || c == '\n') return 0x28;
    if (c == 0x1b) return 0x29;
    if (c == 0x7f || c == '\b') return 0x2a;
    if (c == '\t') return 0x2b;
    if (c == ' ') return 0x2c;
    return 0;
}

/* Special keys by label, for the HID path (arrows and navigation are escape sequences on the serial path). */
static int named_hid(const char *lab) {
    static const struct { const char *l; int code; } N[] = {{"Right", 0x4f}, {"Left", 0x50}, {"Down", 0x51}, {"Up", 0x52},
        {"PgUp", 0x4b}, {"PgDn", 0x4e}, {"Home", 0x4a}, {"End", 0x4d}, {"Esc", 0x29}, {"Tab", 0x2b}, {"Bksp", 0x2a}, {"Enter", 0x28}};
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) if (!strcmp(lab, N[i].l)) return N[i].code;
    return 0;
}

static unsigned long hid_keys;
static int pr_r = -1, pr_c = -1, touch_id = -1;

static void key_rect(int r, int c, int *x, int *y, int *w, int *h) {
    int kh = (fb->h - kb_y) / 6, pad = fb->w / 300 + 3;
    float tot = 0, before = 0;
    for (int i = 0; i < KB_N[r]; i++) { if (i == c) before = tot; tot += KB[r][i].w; }
    float u = (fb->w - pad) / tot;
    *x = (int)(pad + before * u); *w = (int)(KB[r][c].w * u) - pad;
    *y = kb_y + r * kh + pad / 2; *h = kh - pad;
}

static void draw_key(int r, int c) {
    int x, y, w, h;
    key_rect(r, c, &x, &y, &w, &h);
    const struct key *k = &KB[r][c];
    int mod = (k->lab[0] == 'C' && !strcmp(k->lab, "Ctrl") && ctrl) || (!strcmp(k->lab, "Alt") && altk) ||
              (!strcmp(k->lab, "Shift") && shift) || (!strcmp(k->lab, "Sym") && sym) || (!strcmp(k->lab, "Out") && hid_out);
    int down = r == pr_r && c == pr_c;
    if (cv_alloc(&cv, w, h) < 0) return;
    cv_clear(&cv, 0x000000);
    cv_rrect(&cv, 0, 0, w, h, h / 6, mod ? 0x2563eb : down ? 0x3b4252 : 0x1f232b, 255);
    const char *lab = !strcmp(k->lab, "Out") ? (hid_out ? "To: KBD" : "To: TTY") : k->lab;
    char one[8];
    if (k->out && !k->lab[1] && k->lab[0] >= 'a' && k->lab[0] <= 'z') { snprintf(one, sizeof one, "%s", sym ? k->sym : shift ? k->shift : k->lab); lab = one; }
    else if (k->out && strlen(k->lab) <= 2 && (sym || shift) && k->sym && (unsigned char)k->lab[0] < 0x80) { snprintf(one, sizeof one, "%s", sym ? k->sym : k->shift); lab = one; }
    int fs = strlen(lab) > 1 && (unsigned char)lab[0] < 0x80 ? h / 3 : h / 2;
    int fnt = (unsigned char)lab[0] >= 0x80 ? F_MONO : F_REGULAR; /* arrows: Roboto lacks some */
    cv_text(&cv, fnt, fs, (w - cv_text_w(fnt, fs, lab, 0)) / 2, h / 2 + fs * 36 / 100, lab, 0xe5e7eb, 255, 0, w);
    fb_blit(fb, &cv, x, y);
}

static void draw_keyboard(void) {
    if (!kb_shown) return;
    fb_fill(fb, 0, kb_y, fb->w, fb->h - kb_y, fb_color(fb, 0, 0, 0));
    for (int r = 0; r < 6; r++) for (int c = 0; c < KB_N[r]; c++) draw_key(r, c);
}

static void send_key(const struct key *k) {
    if (!strcmp(k->lab, "Ctrl")) { ctrl = !ctrl; draw_keyboard(); return; }
    if (!strcmp(k->lab, "Alt")) { altk = !altk; draw_keyboard(); return; }
    if (!strcmp(k->lab, "Shift")) { shift = !shift; draw_keyboard(); return; }
    if (!strcmp(k->lab, "Sym")) { sym = !sym; draw_keyboard(); return; }
    if (!strcmp(k->lab, "Hide")) { term_toggle_keyboard(); return; }
    if (!strcmp(k->lab, "Out")) { hid_out = !hid_out; logmsg("rescue: typing to %s", hid_out ? "the USB keyboard" : "the serial console"); term_draw(); return; }
    if (hid_out) { /* blind typing on the server's own console */
        int mods = (ctrl ? 0x01 : 0) | (altk ? 0x04 : 0), code = named_hid(k->lab), sh = 0;
        if (!code) {
            const char *o = sym ? k->sym : shift ? k->shift : k->out;
            if (o && strlen(o) == 1) code = ascii_hid((unsigned char)o[0], &sh);
        }
        if (code) { hm_key_tap(mods | (sh ? 0x02 : 0), code); hid_keys++; }
        int redraw = ctrl || altk || (shift && strcmp(k->lab, "Shift"));
        ctrl = altk = 0;
        if (shift && strcmp(k->lab, "Shift")) shift = 0;
        if (redraw) draw_keyboard();
        return;
    }
    if (!strcmp(k->lab, "Size")) { /* tell the server our size (serial lines have none) */
        char b[48];
        int l = snprintf(b, sizeof b, "stty rows %d cols %d\r", rows, cols);
        tty_write(b, (size_t)l);
        return;
    }
    const char *o = sym ? k->sym : shift ? k->shift : k->out;
    if (!o) return;
    char b[16];
    size_t l = 0;
    if (strlen(k->out) == 1 && k->out[0] >= 'A' && k->out[0] <= 'D' && strlen(k->lab) > 1) { /* arrows */
        l = (size_t)snprintf(b, sizeof b, "\x1b%c%c", app_cursor ? 'O' : '[', k->out[0]);
    } else if (ctrl && strlen(o) == 1 && ((o[0] >= 'a' && o[0] <= 'z') || (o[0] >= '@' && o[0] <= '_'))) {
        b[0] = (char)(o[0] & 0x1f); l = 1;
    } else l = (size_t)snprintf(b, sizeof b, "%s", o);
    if (altk) { tty_write("\x1b", 1); altk = 0; }
    tty_write(b, l);
    int redraw = ctrl || (shift && strcmp(k->lab, "Shift"));
    ctrl = 0;
    if (shift && strcmp(k->lab, "Shift")) shift = 0; /* one-shot */
    if (redraw) draw_keyboard();
}

/* ---------------------------------------------------------------- drawing */

static void layout(void) {
    int land = fb->w > fb->h;
    fsz = land ? 34 : 28;
    if (!fsz) fsz = 28;
    cw = cv_text_w(F_MONO, fsz, "M", 0);
    if (cw < 4) cw = fsz * 6 / 10;
    ch_ = fsz * 120 / 100;
    kb_y = kb_shown ? (land ? fb->h * 50 / 100 : fb->h * 62 / 100) : fb->h;
    int nc = (fb->w - 8) / cw, nr = (kb_y - 8) / ch_;
    if (nc > MAXC) nc = MAXC;
    if (nr > MAXR) nr = MAXR;
    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    if (nc != cols || nr != rows) {
        cols = nc; rows = nr;
        top = 0; bot = rows - 1;
        if (cx >= cols) cx = cols - 1;
        if (cy >= rows) { scroll_up(0, cy, cy - rows + 1); cy = rows - 1; }
    }
    tx0 = (fb->w - cols * cw) / 2; ty0 = 4;
}

static void draw_row(int r) {
    int w = cols * cw;
    if (cv_alloc(&cv, w, ch_) < 0) return;
    cv_clear(&cv, DEF_BG);
    int base = fsz * 95 / 100;
    for (int c = 0; c < cols; c++) {
        struct cell e = scr[alt][r][c];
        uint32_t f = e.fg, b = e.bg;
        if (e.at & A_REV) { uint32_t t = f; f = b; b = t; }
        if (cursor_on && r == cy && c == cx && active) { uint32_t t = f; f = b == DEF_BG ? DEF_BG : b; b = t == DEF_BG ? DEF_FG : t; }
        if (e.at & A_BOLD) f = rgb_mix(f, 0xffffff, 60);
        if (e.at & A_DIM) f = rgb_mix(DEF_BG, f, 150);
        if (b != DEF_BG) cv_rect(&cv, c * cw, 0, cw, ch_, b, 255);
        if (e.ch > ' ') {
            char u[5];
            uint32_t cp = e.ch;
            if (cp < 0x80) { u[0] = (char)cp; u[1] = 0; }
            else if (cp < 0x800) { u[0] = (char)(0xc0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); u[2] = 0; }
            else { u[0] = (char)(0xe0 | cp >> 12); u[1] = (char)(0x80 | (cp >> 6 & 63)); u[2] = (char)(0x80 | (cp & 63)); u[3] = 0; }
            cv_text(&cv, F_MONO, fsz, c * cw, base, u, f, 255, 0, 0);
        }
        if (e.at & A_UL) cv_rect(&cv, c * cw, ch_ - 3, cw, 2, f, 255);
    }
    fb_blit(fb, &cv, tx0, ty0 + r * ch_);
}

static void draw_dirty(void) {
    if (!active || !fb) return;
    for (int r = 0; r < rows; r++) if (dirty[r]) { draw_row(r); dirty[r] = 0; }
    fb_present(fb);
}

void term_draw(void) {
    if (!fb || !fb->mem) return;
    layout();
    fb_fill(fb, 0, 0, fb->w, kb_y, fb_color(fb, DEF_BG >> 16, DEF_BG >> 8 & 255, DEF_BG & 255));
    mark_all();
    for (int r = 0; r < rows; r++) { draw_row(r); dirty[r] = 0; }
    if (fd < 0) {
        if (cv_alloc(&cv, fb->w, 60) == 0) {
            cv_clear(&cv, DEF_BG);
            cv_text(&cv, F_REGULAR, 32, 20, 44, "Console port not available", 0xef4444, 255, 0, 0);
            fb_blit(fb, &cv, 0, kb_y - 70);
        }
    }
    if (remote && cv_alloc(&cv, fb->w, 56) == 0) {
        cv_clear(&cv, 0x7c3aed);
        cv_text(&cv, F_MEDIUM, 30, 20, 40, "A remote SSH session is using the server console", 0xffffff, 255, 0, fb->w - 40);
        fb_blit(fb, &cv, 0, 4);
    }
    if (hid_out && cv_alloc(&cv, fb->w, 56) == 0) {
        cv_clear(&cv, 0x2563eb);
        cv_text(&cv, F_MEDIUM, 30, 20, 40, "Typing on the server's USB keyboard (blind) - tap To: KBD to go back", 0xffffff, 255, 0, fb->w - 40);
        fb_blit(fb, &cv, 0, kb_y - 56);
    }
    draw_keyboard();
    fb_present(fb);
}

void term_toggle_keyboard(void) { kb_shown = !kb_shown; term_draw(); }

/* ---------------------------------------------------------------- life cycle (main loop) */

int term_fd(void) { return active ? fd : -1; }

void term_read(void) {
    unsigned char b[4096];
    int prev_cx = cx, prev_cy = cy;
    for (;;) {
        ssize_t n = read(fd, b, sizeof b);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { con_open(); break; } /* hung up */
        if (n < 0) break;
        bytes_in += (unsigned long)n;
        for (ssize_t i = 0; i < n; i++) feed(b[i]);
    }
    mark(prev_cy); mark(cy);
    (void)prev_cx;
    draw_dirty();
}

/* Enter/leave RESCUE: takes the shell port from PhoneKit's debug shell, or gives it back. */
void term_activate(int on, const char *console_tty) {
    if (on == active) { if (on) term_draw(); return; }
    active = on;
    if (on) {
        snprintf(tty, sizeof tty, "%s", console_tty ? console_tty : "");
        shell_pause(1);
        con_open();
        logmsg("rescue: console on %s (%s)", tty, fd >= 0 ? "open" : strerror(errno));
        term_draw();
    } else {
        if (fd >= 0) close(fd);
        fd = -1;
        shell_pause(0);
        logmsg("rescue: console released, debug shell back");
    }
}

int term_active(void) { return active; }

/* A remote SSH session (`pk-console`) has the console while /run/rescue.remote exists: step aside, then take it back. */
void term_tick(void) {
    if (!active) return;
    int r = access("/run/rescue.remote", F_OK) == 0;
    if (r == remote) return;
    remote = r;
    if (r) { if (fd >= 0) close(fd); fd = -1; logmsg("rescue: console handed to a remote session"); }
    else { con_open(); logmsg("rescue: console back from the remote session"); }
    term_draw();
}

static int key_at(int x, int y, int *rr, int *cc) {
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < KB_N[r]; c++) {
            int kx, ky, kw, kh;
            key_rect(r, c, &kx, &ky, &kw, &kh);
            if (x >= kx && x < kx + kw && y >= ky && y < ky + kh) { *rr = r; *cc = c; return 1; }
        }
    return 0;
}

/* One finger at a time. The key under the finger is highlighted as it moves and fires where the finger
 * lifts (as phone keyboards do); a frame with no fingers is the lift. */
int term_touch(const int *ids, const int *xs, const int *ys, int n) {
    static int lx = -1, ly = -1;
    if (n == 0) {
        touch_id = -1;
        int r = -1, c = -1, had = pr_r >= 0;
        if (lx >= 0 && kb_shown) key_at(lx, ly, &r, &c);
        if (had) { int pr = pr_r, pc = pr_c; pr_r = pr_c = -1; draw_key(pr, pc); }
        if (!kb_shown && ly > fb->h * 85 / 100) term_toggle_keyboard(); /* tap the bottom to bring it back */
        else if (r >= 0) { send_key(&KB[r][c]); fb_present(fb); }
        else if (had) fb_present(fb);
        lx = ly = -1;
        return 1;
    }
    touch_id = ids[0];
    lx = xs[0]; ly = ys[0];
    int r, c;
    if (kb_shown && key_at(lx, ly, &r, &c) && (r != pr_r || c != pr_c)) {
        int pr = pr_r, pc = pr_c;
        pr_r = r; pr_c = c;
        if (pr >= 0) draw_key(pr, pc);
        draw_key(r, c);
        fb_present(fb);
    }
    return 1;
}

int term_stats(char *b, size_t len) {
    return snprintf(b, len, "active %d\ntty %s\nfd %d\ngrid %dx%d\ncursor %d,%d\nalt %d\nbytes_in %lu\nbytes_out %lu\nout %s\nhid_keys %lu\n",
                    active, tty, fd, cols, rows, cx, cy, alt, bytes_in, bytes_out, hid_out ? "usb-keyboard" : "serial", hid_keys);
}

/* Types ASCII text as USB/Bluetooth keyboard presses (ctl "bt type"; tests and DECK macros). */
void type_hid_text(const char *s) {
    for (; *s; s++) {
        int sh = 0, code = ascii_hid((unsigned char)*s, &sh);
        if (code) hm_key_tap(sh ? 0x02 : 0, code);
    }
}

/* Types text into the console (ctl "rescue type"; tests). */
void term_type(const char *s) { tty_write(s, strlen(s)); }

void term_init(struct fb *f) { fb = f; reset(); }
