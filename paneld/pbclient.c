// PHONEBOX mode: a native client for the phonebox desktop agent (github.com/ersinkoc/phonebox).
//
// The agent tunnels its HTTP server to the phone with `adb reverse tcp:8765` (served by adbd.c). This file
// reads the agent's page config (/api/config) and live state (/api/stream, server-sent events) from
// 127.0.0.1:8765 and turns them into PPJ/1 layouts and value updates for the native widget engine (panel.c).
// Taps on shortcut and media buttons go back as the same POSTs the phonebox app sends.
//
// A background thread does the blocking HTTP; the main loop (pb_handle) does all parsing and drawing.
#include "paneld.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AGENT_PORT 8765
#define APP_VERSION "99.0.0" /* never older than the agent's APK: no update offers to a phone without Android */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *cfg_json, *state_json; /* latest bodies, handed to the main loop */
static int cfg_new, state_new;
static int notify_r = -1, notify_w = -1;
static volatile int connected;
static long long cfg_version;
static unsigned long n_states, n_cfgs, n_posts;

/* ---------------------------------------------------------------- HTTP (background thread) */

struct conn { int fd; char buf[8192]; int len, pos; int chunked; long remain; /* body bytes left (-1: until close) */ };

static int conn_open(struct conn *c) {
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0) return -1;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(AGENT_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct timeval tv = {15, 0}; /* the stream sends a state every second */
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(c->fd); c->fd = -1; return -1; }
    return 0;
}

static int raw_getc(struct conn *c) {
    if (c->pos >= c->len) {
        ssize_t n = recv(c->fd, c->buf, sizeof c->buf, 0);
        if (n <= 0) return -1;
        c->len = (int)n; c->pos = 0;
    }
    return (unsigned char)c->buf[c->pos++];
}

static int raw_line(struct conn *c, char *out, int max) {
    int n = 0, ch;
    while ((ch = raw_getc(c)) >= 0) {
        if (ch == '\n') break;
        if (ch != '\r' && n < max - 1) out[n++] = (char)ch;
    }
    out[n] = 0;
    return ch < 0 && !n ? -1 : n;
}

/* One body byte, decoding chunked transfer encoding. */
static int body_getc(struct conn *c) {
    if (c->chunked) {
        if (c->remain == 0) {
            char l[32];
            if (raw_line(c, l, sizeof l) < 0) return -1;
            if (!l[0] && raw_line(c, l, sizeof l) < 0) return -1; /* CRLF after the previous chunk */
            c->remain = strtol(l, NULL, 16);
            if (c->remain <= 0) return -1; /* last chunk */
        }
        c->remain--;
        return raw_getc(c);
    }
    if (c->remain == 0) return -1;
    if (c->remain > 0) c->remain--;
    return raw_getc(c);
}

static int body_line(struct conn *c, char *out, int max) {
    int n = 0, ch;
    while ((ch = body_getc(c)) >= 0) {
        if (ch == '\n') break;
        if (ch != '\r' && n < max - 1) out[n++] = (char)ch;
    }
    out[n] = 0;
    return ch < 0 && !n ? -1 : n;
}

static int request(struct conn *c, const char *method, const char *path) {
    if (conn_open(c) < 0) return -1;
    char rq[512];
    int n = snprintf(rq, sizeof rq,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-PhoneBox-App: %s\r\nX-PhoneBox-Device: phonekit0001\r\n"
                     "X-PhoneBox-Model: PhoneKit\r\nAccept: */*\r\nConnection: close\r\n%s\r\n",
                     method, path, AGENT_PORT, APP_VERSION, !strcmp(method, "POST") ? "Content-Length: 0\r\n" : "");
    if (send(c->fd, rq, (size_t)n, MSG_NOSIGNAL) != n) { close(c->fd); return -1; }
    char l[512];
    if (raw_line(c, l, sizeof l) < 0) { close(c->fd); return -1; }
    int code = 0;
    sscanf(l, "HTTP/%*s %d", &code);
    c->remain = -1;
    while (raw_line(c, l, sizeof l) > 0) {
        if (!strncasecmp(l, "transfer-encoding:", 18) && strcasestr(l, "chunked")) { c->chunked = 1; c->remain = 0; }
        else if (!strncasecmp(l, "content-length:", 15)) c->remain = atol(l + 15);
    }
    return code;
}

static char *read_body(struct conn *c, size_t max) {
    size_t cap = 16384, n = 0;
    char *b = malloc(cap);
    int ch;
    while (b && (ch = body_getc(c)) >= 0 && n < max) {
        if (n + 1 >= cap) { cap *= 2; char *nb = realloc(b, cap); if (!nb) { free(b); return NULL; } b = nb; }
        b[n++] = (char)ch;
    }
    if (b) b[n] = 0;
    return b;
}

static void publish(char **slot, int *flag, char *body) {
    pthread_mutex_lock(&lock);
    free(*slot);
    *slot = body;
    *flag = 1;
    pthread_mutex_unlock(&lock);
    char x = 1;
    if (notify_w >= 0) (void)!write(notify_w, &x, 1);
}

static long long json_ll_after(const char *s, const char *key) {
    const char *p = strstr(s, key);
    return p ? atoll(p + strlen(key)) : -1;
}

static void *http_thread(void *v) {
    (void)v;
    char *line = malloc(256 * 1024);
    for (;;) {
        if (!adbd_reverse_active(AGENT_PORT)) { connected = 0; sleep(1); continue; }
        struct conn c;
        if (request(&c, "GET", "/api/config") != 200) { if (c.fd >= 0) close(c.fd); connected = 0; sleep(2); continue; }
        char *cfg = read_body(&c, 4 << 20);
        close(c.fd);
        if (!cfg) { sleep(2); continue; }
        cfg_version = json_ll_after(cfg, "\"version\":");
        n_cfgs++;
        publish(&cfg_json, &cfg_new, cfg);
        if (request(&c, "GET", "/api/stream") != 200) { if (c.fd >= 0) close(c.fd); sleep(2); continue; }
        connected = 1;
        logmsg("phonebox: live stream open (config v%lld)", cfg_version);
        char ev[32] = "message";
        for (;;) {
            int n = body_line(&c, line, 256 * 1024);
            if (n < 0) break;
            if (!strncmp(line, "event: ", 7)) snprintf(ev, sizeof ev, "%s", line + 7);
            else if (!strncmp(line, "data: ", 6) && !strcmp(ev, "state")) {
                n_states++;
                long long v = json_ll_after(line, "\"cfgVersion\":");
                publish(&state_json, &state_new, strdup(line + 6));
                if (v > 0 && v != cfg_version) break; /* layout edited on the PC */
            } else if (!n) snprintf(ev, sizeof ev, "message");
        }
        close(c.fd);
        connected = 0;
        logmsg("phonebox: stream closed");
        usleep(300000);
    }
    return NULL;
}

struct post_arg { char path[160]; };
static void *post_thread(void *v) {
    struct post_arg *a = v;
    struct conn c;
    int code = request(&c, "POST", a->path);
    if (c.fd >= 0) close(c.fd);
    logmsg("phonebox: POST %s -> %d", a->path, code);
    n_posts++;
    free(a);
    return NULL;
}

static void post(const char *path) {
    struct post_arg *a = calloc(1, sizeof *a);
    snprintf(a->path, sizeof a->path, "%s", path);
    pthread_t t;
    if (pthread_create(&t, NULL, post_thread, a)) { free(a); return; }
    pthread_detach(t);
}

/* ---------------------------------------------------------------- JSON helpers (main loop) */

#define MAXTOK 16384
static jtok tk[MAXTOK];

/* Value token at a dotted path ("pc.gpu.util") under object `o`, or -1. */
static int jpath(const char *s, int n, int o, const char *path) {
    char key[32];
    while (*path && o >= 0) {
        const char *dot = strchr(path, '.');
        size_t l = dot ? (size_t)(dot - path) : strlen(path);
        if (l >= sizeof key) return -1;
        memcpy(key, path, l); key[l] = 0;
        o = json_get(s, tk, n, o, key);
        path = dot ? dot + 1 : path + l;
    }
    return o;
}

static int is_null(const char *s, int i) { return i < 0 || (tk[i].type == J_PRIM && s[tk[i].start] == 'n'); }

static double num(const char *s, int n, int o, const char *path, double d) {
    int i = jpath(s, n, o, path);
    return is_null(s, i) ? d : json_num(s, tk, i, d);
}

/* Escapes text for a JSON string we generate (ASCII only: the font has no more). */
static void jesc(char *out, size_t len, const char *in) {
    size_t o = 0;
    for (; *in && o + 2 < len; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch >= 0x80) { while ((in[1] & 0xc0) == 0x80) in++; ch = '?'; }
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch >= 32) out[o++] = (char)ch;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- layout (main loop) */

/* What each PPJ widget shows; filled when the layout is built, used on every state. */
enum { V_NONE, V_CPU, V_GPU, V_RAM, V_RAMPCT, V_VRAM, V_DOWN, V_UP, V_PING, V_GPUTEMP, V_CPUTEMP, V_DISK0, V_DISK1,
       V_MEDIA_TITLE, V_MEDIA_ARTIST, V_MEDIA_PLAY };
struct bind { char id[24]; int what; int graph; };
static struct bind binds[8 * 48];
static int nbinds;
static int built_landscape = -1;
static char *cfg_copy; /* the config we built from, kept to rebuild on rotation */

static int metric_of(const char *m) {
    static const struct { const char *n; int v; } M[] = {
        {"cpu", V_CPU}, {"gpu", V_GPU}, {"ram", V_RAM}, {"vram", V_VRAM}, {"net", V_DOWN}, {"down", V_DOWN},
        {"up", V_UP}, {"ping", V_PING}, {"gputemp", V_GPUTEMP}, {"cputemp", V_CPUTEMP}};
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (!strcmp(M[i].n, m)) return M[i].v;
    return V_CPU;
}

static const char *metric_label(int v) {
    switch (v) {
    case V_CPU: return "CPU"; case V_GPU: return "GPU"; case V_RAM: return "RAM"; case V_VRAM: return "VRAM";
    case V_DOWN: return "DOWNLOAD"; case V_UP: return "UPLOAD"; case V_PING: return "PING";
    case V_GPUTEMP: return "GPU TEMP"; case V_CPUTEMP: return "CPU TEMP";
    default: return "";
    }
}

static const char *metric_color(int v) {
    switch (v) {
    case V_CPU: return "#38bdf8"; case V_GPU: case V_VRAM: return "#a78bfa"; case V_RAM: case V_RAMPCT: return "#34d399";
    case V_DOWN: return "#22d3ee"; case V_UP: return "#f472b6"; case V_GPUTEMP: case V_CPUTEMP: return "#f97316";
    default: return "#94a3b8";
    }
}

static char lay[48 * 1024];
static size_t lo;
static int lw; /* widgets in the current page */
static int land;

static void bind_add(const char *id, int what, int graph) {
    if (nbinds >= (int)(sizeof binds / sizeof binds[0])) return;
    snprintf(binds[nbinds].id, sizeof binds[nbinds].id, "%s", id);
    binds[nbinds].what = what; binds[nbinds].graph = graph;
    nbinds++;
}

/* Adds one PPJ widget, in phonebox (landscape 12x6) grid coordinates; transposed in portrait. */
static void add(const char *id, const char *type, int x, int y, int w, int h, const char *extra) {
    if (lw >= 48 || w <= 0 || h <= 0) return;
    int X = x, Y = y, W = w, H = h;
    if (!land) { X = y; Y = x; W = h; H = w; }
    lo += (size_t)snprintf(lay + lo, sizeof lay - lo, "%s{\"id\":\"%s\",\"type\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d%s%s}",
                           lw ? "," : "", id, type, X, Y, W, H, extra && *extra ? "," : "", extra ? extra : "");
    lw++;
}

static void build_widget(const char *s, int n, int wtok, int pg, int sc_arr) {
    char type[24] = "", wid[16] = "", id[24], ex[256];
    json_str(s, tk, json_get(s, tk, n, wtok, "type"), type, sizeof type);
    json_str(s, tk, json_get(s, tk, n, wtok, "id"), wid, sizeof wid);
    int x = (int)num(s, n, wtok, "x", 0), y = (int)num(s, n, wtok, "y", 0);
    int w = (int)num(s, n, wtok, "w", 1), h = (int)num(s, n, wtok, "h", 1);
    int opt = json_get(s, tk, n, wtok, "options");
    if (x < 0 || y < 0 || x + w > 12 || y + h > 6) return;
    snprintf(id, sizeof id, "pb.%d.%.12s", pg, wid);
    if (!strcmp(type, "clock") || !strcmp(type, "worldclock") || !strcmp(type, "wordclock") || !strcmp(type, "analog")) {
        add(id, "clock", x, y, w, h, w >= 6 ? "\"format\":\"hms\"" : "\"format\":\"hm\"");
    } else if (!strcmp(type, "stat") || !strcmp(type, "gauge")) {
        char m[16] = "cpu";
        json_str(s, tk, jpath(s, n, opt, "metric"), m, sizeof m);
        int v = metric_of(m);
        snprintf(ex, sizeof ex, "\"label\":\"%s\",\"color\":\"%s\",\"decimals\":%d%s", metric_label(v), metric_color(v),
                 v == V_RAM ? 1 : 0, !strcmp(type, "gauge") ? ",\"min\":0,\"max\":100" : "");
        add(id, !strcmp(type, "gauge") ? "gauge" : "value", x, y, w, h, ex);
        bind_add(id, v, 0);
    } else if (!strcmp(type, "chart")) {
        int arr = jpath(s, n, opt, "metrics");
        char m[16] = "cpu";
        if (arr >= 0 && tk[arr].type == J_ARR && tk[arr].size > 0) json_str(s, tk, arr + 1, m, sizeof m);
        int v = metric_of(m);
        snprintf(ex, sizeof ex, "\"label\":\"%s\",\"color\":\"%s\"", v == V_DOWN || v == V_UP ? "NETWORK MB/S" : metric_label(v),
                 metric_color(v));
        add(id, "graph", x, y, w, h, ex);
        bind_add(id, v, 1);
    } else if (!strcmp(type, "meters")) {
        static const int rows_what[] = {V_RAMPCT, V_VRAM, V_DISK0, V_DISK1};
        static const char *const rows_lbl[] = {"RAM", "VRAM", "DISK", "DISK 2"};
        int k = h < 4 ? h : 4;
        for (int i = 0; i < k; i++) {
            char sid[24];
            snprintf(sid, sizeof sid, "%s.%d", id, i);
            snprintf(ex, sizeof ex, "\"label\":\"%s\",\"unit\":\"%%\",\"min\":0,\"max\":100,\"color\":\"%s\"", rows_lbl[i],
                     metric_color(rows_what[i]));
            add(sid, "gauge", x, y + i * (h / k), w, h / k, ex);
            bind_add(sid, rows_what[i], 0);
        }
    } else if (!strcmp(type, "media")) {
        char sid[24];
        int th = h >= 3 ? h - 1 : h;
        snprintf(sid, sizeof sid, "%s.t", id);
        add(sid, "label", x, y, w, th > 1 ? th - 1 : 1, "\"text\":\"NOTHING PLAYING\",\"size\":3");
        bind_add(sid, V_MEDIA_TITLE, 0);
        if (th > 1) {
            snprintf(sid, sizeof sid, "%s.a", id);
            add(sid, "label", x, y + th - 1, w, 1, "\"text\":\"\",\"size\":2");
            bind_add(sid, V_MEDIA_ARTIST, 0);
        }
        if (h >= 3) {
            int bw = w / 3;
            add("pb.media.prev", "button", x, y + h - 1, bw, 1, "\"text\":\"PREV\"");
            add("pb.media.playpause", "button", x + bw, y + h - 1, w - 2 * bw, 1, "\"text\":\"PLAY\"");
            add("pb.media.next", "button", x + w - bw, y + h - 1, bw, 1, "\"text\":\"NEXT\"");
            bind_add("pb.media.playpause", V_MEDIA_PLAY, 0);
        }
    } else if (!strcmp(type, "shortcuts")) {
        int ns = sc_arr >= 0 && tk[sc_arr].type == J_ARR ? tk[sc_arr].size : 0;
        int cols = (int)num(s, n, opt, "columns", 4);
        if (cols < 1) cols = 1;
        if (cols > w) cols = w;
        int cw = w / cols, rows = (ns + cols - 1) / cols;
        if (rows < 1) rows = 1;
        int rh = h / rows;
        if (rh < 1) { rh = 1; rows = h; }
        int j = sc_arr + 1;
        for (int i = 0; i < ns && i < cols * rows; i++, j = json_skip(tk, n, j)) {
            char sid[24], scid[17] = "", label[40] = "", el[48];
            json_str(s, tk, json_get(s, tk, n, j, "id"), scid, sizeof scid);
            json_str(s, tk, json_get(s, tk, n, j, "label"), label, sizeof label);
            jesc(el, sizeof el, label);
            snprintf(sid, sizeof sid, "pb.run.%s", scid);
            char col[16] = "";
            json_str(s, tk, json_get(s, tk, n, j, "color"), col, sizeof col);
            snprintf(ex, sizeof ex, "\"text\":\"%.20s\"%s%s%s", el, col[0] == '#' ? ",\"color\":\"" : "", col[0] == '#' ? col : "",
                     col[0] == '#' ? "\"" : "");
            add(sid, "button", x + (i % cols) * cw, y + (i / cols) * rh, cw, rh, ex);
        }
    } else if (!strcmp(type, "label") || !strcmp(type, "note") || !strcmp(type, "quote")) {
        char txt[64] = "", e[80];
        json_str(s, tk, jpath(s, n, opt, "text"), txt, sizeof txt);
        jesc(e, sizeof e, txt[0] ? txt : type);
        snprintf(ex, sizeof ex, "\"text\":\"%.40s\",\"size\":2", e);
        add(id, "label", x, y, w, h, ex);
    } else {
        /* not drawn natively yet: say so in its place */
        char up[32];
        size_t i = 0;
        for (; type[i] && i < sizeof up - 1; i++) up[i] = (char)(type[i] >= 'a' && type[i] <= 'z' ? type[i] - 32 : type[i]);
        up[i] = 0;
        snprintf(ex, sizeof ex, "\"text\":\"%s (SOON)\",\"size\":2,\"color\":\"#475569\"", up);
        add(id, "label", x, y, w, h, ex);
    }
}

static void build_layout(const char *cfg) {
    int n = json_parse(cfg, (int)strlen(cfg), tk, MAXTOK);
    if (n < 1) { logmsg("phonebox: config JSON unparsable (%d)", n); return; }
    int c = json_get(cfg, tk, n, 0, "config");
    int pages = jpath(cfg, n, c, "pages"), sc = jpath(cfg, n, c, "shortcuts");
    if (pages < 0 || tk[pages].type != J_ARR) return;
    land = panel_landscape();
    built_landscape = land;
    nbinds = 0;
    char rep[256];
    panel_handle("{\"op\":\"clear\"}", 14, rep, sizeof rep);
    int pg = 0, j = pages + 1;
    for (int k = 0; k < tk[pages].size && pg < 8; k++, j = json_skip(tk, n, j)) {
        if (tk[j].type != J_OBJ) continue;
        int en = json_get(cfg, tk, n, j, "enabled");
        if (en >= 0 && cfg[tk[en].start] == 'f') continue;
        char title[32] = "", et[40];
        json_str(cfg, tk, json_get(cfg, tk, n, j, "title"), title, sizeof title);
        jesc(et, sizeof et, title);
        lo = 0; lw = 0;
        lo += (size_t)snprintf(lay, sizeof lay, "{\"op\":\"layout\",\"page\":%d,\"title\":\"%.24s\",\"widgets\":[", pg, et);
        int ws = json_get(cfg, tk, n, j, "widgets");
        if (ws >= 0 && tk[ws].type == J_ARR) {
            int q = ws + 1;
            for (int i = 0; i < tk[ws].size; i++, q = json_skip(tk, n, q))
                if (tk[q].type == J_OBJ) build_widget(cfg, n, q, pg, sc);
        }
        lo += (size_t)snprintf(lay + lo, sizeof lay - lo, "]}");
        panel_handle(lay, (int)lo, rep, sizeof rep); /* panel.c has its own token array: tk stays valid */
        pg++;
    }
    char p0[32];
    int l0 = snprintf(p0, sizeof p0, "{\"op\":\"page\",\"n\":0}");
    panel_handle(p0, l0, rep, sizeof rep);
    logmsg("phonebox: %d pages built (%s)", pg, land ? "landscape" : "portrait, transposed");
}

/* ---------------------------------------------------------------- live state (main loop) */

static void set_num(const char *id, double v, const char *unit, int graph) {
    char b[160], rep[64];
    int n = graph ? snprintf(b, sizeof b, "{\"op\":\"set\",\"id\":\"%s\",\"push\":%.3f}", id, v)
                  : snprintf(b, sizeof b, "{\"op\":\"set\",\"id\":\"%s\",\"value\":%.3f%s%s%s}", id, v,
                             unit ? ",\"unit\":\"" : "", unit ? unit : "", unit ? "\"" : "");
    panel_handle(b, n, rep, sizeof rep);
}

static void set_text(const char *id, const char *field, const char *txt) {
    char e[96], b[200], rep[64];
    jesc(e, sizeof e, txt);
    int n = snprintf(b, sizeof b, "{\"op\":\"set\",\"id\":\"%s\",\"%s\":\"%.44s\"}", id, field, e);
    panel_handle(b, n, rep, sizeof rep);
}

static void apply_state(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    if (n < 1) return;
    double ram = num(s, n, 0, "pc.ramUsedGb", 0), ramt = num(s, n, 0, "pc.ramTotalGb", 1);
    double vu = num(s, n, 0, "pc.gpu.vramUsed", 0), vt = num(s, n, 0, "pc.gpu.vramTotal", 0);
    int disks = jpath(s, n, 0, "pc.disks");
    double dpct[2] = {0, 0};
    if (disks >= 0 && tk[disks].type == J_ARR) {
        int q = disks + 1;
        for (int i = 0; i < tk[disks].size && i < 2; i++, q = json_skip(tk, n, q)) {
            double u = num(s, n, q, "usedGb", 0), t = num(s, n, q, "totalGb", 1);
            dpct[i] = t > 0 ? u / t * 100 : 0;
        }
    }
    int media = json_get(s, tk, n, 0, "media");
    char title[96] = "", artist[96] = "";
    int playing = 0;
    if (!is_null(s, media)) {
        json_str(s, tk, json_get(s, tk, n, media, "title"), title, sizeof title);
        json_str(s, tk, json_get(s, tk, n, media, "artist"), artist, sizeof artist);
        playing = num(s, n, media, "playing", 0) > 0;
    }
    for (int i = 0; i < nbinds; i++) {
        struct bind *b = &binds[i];
        double v = 0;
        const char *unit = NULL;
        switch (b->what) {
        case V_CPU: v = num(s, n, 0, "pc.cpu", 0); unit = "%"; break;
        case V_GPU: v = num(s, n, 0, "pc.gpu.util", 0); unit = "%"; break;
        case V_RAM: v = ram; unit = "GB"; break;
        case V_RAMPCT: v = ramt > 0 ? ram / ramt * 100 : 0; break;
        case V_VRAM: v = vt > 0 ? vu / vt * 100 : 0; unit = "%"; break;
        case V_DOWN: case V_UP: {
            double bps = num(s, n, 0, b->what == V_DOWN ? "pc.netDownBps" : "pc.netUpBps", 0);
            if (b->graph) v = bps / 1e6;
            else if (bps >= 1e6) { v = bps / 1e6; unit = "MB/S"; }
            else { v = bps / 1e3; unit = "KB/S"; }
            break;
        }
        case V_PING: v = num(s, n, 0, "ping.ms", 0); unit = "MS"; break;
        case V_GPUTEMP: v = num(s, n, 0, "pc.gpu.temp", 0); unit = "C"; break;
        case V_CPUTEMP: v = num(s, n, 0, "pc.cpuTemp", 0); unit = "C"; break;
        case V_DISK0: v = dpct[0]; break;
        case V_DISK1: v = dpct[1]; break;
        case V_MEDIA_TITLE: set_text(b->id, "text", title[0] ? title : "NOTHING PLAYING"); continue;
        case V_MEDIA_ARTIST: set_text(b->id, "text", artist); continue;
        case V_MEDIA_PLAY: set_text(b->id, "text", playing ? "PAUSE" : "PLAY"); continue;
        default: continue;
        }
        set_num(b->id, v, b->graph ? NULL : unit, b->graph);
    }
}

/* ---------------------------------------------------------------- main-loop API */

int pb_fd(void) { return notify_r; }

void pb_handle(void) {
    char x[64];
    while (read(notify_r, x, sizeof x) > 0) {}
    pthread_mutex_lock(&lock);
    char *cfg = cfg_new ? cfg_json : NULL, *st = state_new ? state_json : NULL;
    if (cfg) { cfg_json = NULL; cfg_new = 0; }
    if (st) { state_json = NULL; state_new = 0; }
    pthread_mutex_unlock(&lock);
    if (cfg) { free(cfg_copy); cfg_copy = cfg; build_layout(cfg_copy); }
    if (st) { if (cfg_copy) apply_state(st); free(st); }
}

/* Once a second from the main loop: re-flow after a rotation. */
void pb_tick(void) {
    if (cfg_copy && built_landscape >= 0 && panel_landscape() != built_landscape) build_layout(cfg_copy);
}

/* A tap on one of our buttons (ids "pb.run.<shortcut>", "pb.media.<action>"). Returns 1 if it was ours. */
int pb_event(const char *id) {
    char path[160];
    if (!strncmp(id, "pb.run.", 7)) snprintf(path, sizeof path, "/api/run/%s", id + 7);
    else if (!strncmp(id, "pb.media.", 9)) snprintf(path, sizeof path, "/api/media/%s", id + 9);
    else return !strncmp(id, "pb.", 3);
    post(path);
    return 1;
}

int pb_stats(char *b, size_t len) {
    return snprintf(b, len, "connected %d\nconfig_version %lld\nconfigs %lu\nstates %lu\nposts %lu\nbinds %d\n",
                    connected, cfg_version, n_cfgs, n_states, n_posts, nbinds);
}

void pb_init(void) {
    int pf[2];
    if (pipe2(pf, O_CLOEXEC | O_NONBLOCK) < 0) return;
    notify_r = pf[0]; notify_w = pf[1];
    pthread_t t;
    pthread_create(&t, NULL, http_thread, NULL);
    pthread_detach(t);
}
