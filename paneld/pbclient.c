// PHONEBOX mode: a native client for the phonebox desktop agent (github.com/ersinkoc/phonebox).
//
// The agent tunnels its HTTP server to the phone with `adb reverse tcp:8765` (served by adbd.c). This file
// reads the agent's page config (/api/config), live state (/api/stream, server-sent events), album art
// (/api/art) and weather (open-meteo through the agent's /api/proxy) from 127.0.0.1:8765, and draws the pages
// natively in phonebox's look (its CSS tokens and sizes; the phone's landscape 2340x1080 is exactly 3x phonebox's
// 780x360 CSS layout). Taps on shortcut and media buttons go back as the same POSTs the phonebox app sends.
//
// A background thread does the blocking HTTP; the main loop (pb_handle/pb_tick/pb_touch) parses and draws.
#include "paneld.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <bearssl.h>

#define AGENT_PORT 8765
#define APP_VERSION "99.0.0" /* never older than the agent's APK: no update offers to a phone without Android */

/* ================================================================ HTTP (background threads) */

struct blob { char *data; size_t len; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct blob in_cfg, in_state, in_art, in_wx, in_work, in_quota, in_procs;
#define EVQ 16
static char *evq[EVQ];            /* panel events from the stream ({"kind","data"}), oldest first */
static int evn;
static volatile int want_approvals; /* main loop: fetch /api/approvals */
static struct blob in_appr;
static volatile int want_quotas, want_procs; /* main loop: a visible/configured widget needs it */ /* latest bodies, handed to the main loop */
static int notify_r = -1, notify_w = -1;
static volatile int connected;
static volatile long art_want = -1;      /* main loop asks for this media artId */
static volatile int wx_want;             /* main loop asks for a weather refresh */
static char wx_query[256];               /* lat/lon from the config */
static long long cfg_version;
static unsigned long n_states, n_cfgs, n_posts;

/* Route to the agent: USB (plain HTTP through the adb reverse tunnel to 127.0.0.1:8765) or Wi-Fi (HTTPS to the
 * PC's address, the agent's self-signed certificate pinned by its SHA-256 fingerprint, plus the bearer token): the
 * same two routes as the phonebox app (AgentLink.kt). The Wi-Fi details come from /api/pair over USB ("pairing"). */
struct route { int wifi; uint32_t ip; int port; char token[96]; unsigned char fp[32]; };
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
static struct route cur_route;
static volatile int route_wifi;  /* for stats */
static volatile int wifi_stream_fd = -1; /* the open Wi-Fi stream's socket, so the main loop can cut it */
static volatile long stream_opened_ms;

/* X.509 "engine" that trusts exactly one certificate: the leaf whose SHA-256 is the pinned fingerprint. */
struct pin_ctx {
    const br_x509_class *vtable;
    br_sha256_context sha;
    br_x509_decoder_context dec;
    unsigned char want[32];
    int depth, ok;
};
static void pin_start_chain(const br_x509_class **ctx, const char *server_name) { (void)server_name; ((struct pin_ctx *)(void *)ctx)->depth = 0; }
static void pin_start_cert(const br_x509_class **ctx, uint32_t length) {
    struct pin_ctx *p = (struct pin_ctx *)(void *)ctx;
    (void)length;
    if (p->depth == 0) { br_sha256_init(&p->sha); br_x509_decoder_init(&p->dec, NULL, NULL); }
}
static void pin_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    struct pin_ctx *p = (struct pin_ctx *)(void *)ctx;
    if (p->depth == 0) { br_sha256_update(&p->sha, buf, len); br_x509_decoder_push(&p->dec, buf, len); }
}
static void pin_end_cert(const br_x509_class **ctx) { ((struct pin_ctx *)(void *)ctx)->depth++; }
static unsigned pin_end_chain(const br_x509_class **ctx) {
    struct pin_ctx *p = (struct pin_ctx *)(void *)ctx;
    unsigned char got[32];
    br_sha256_out(&p->sha, got);
    p->ok = !memcmp(got, p->want, 32) && br_x509_decoder_last_error(&p->dec) == 0 && br_x509_decoder_get_pkey(&p->dec);
    return p->ok ? 0 : BR_ERR_X509_NOT_TRUSTED;
}
static const br_x509_pkey *pin_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    struct pin_ctx *p = (struct pin_ctx *)(void *)ctx;
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return p->ok ? br_x509_decoder_get_pkey(&p->dec) : NULL;
}
static const br_x509_class pin_vtable = {sizeof(struct pin_ctx), pin_start_chain, pin_start_cert, pin_append, pin_end_cert, pin_end_chain, pin_get_pkey};

struct conn {
    int fd; char buf[8192]; int len, pos; int chunked; long remain;
    int tls;
    br_ssl_client_context *sc; br_x509_minimal_context *xm; struct pin_ctx *pin; br_sslio_context io; unsigned char *iobuf;
    char auth[128];
};

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    for (;;) { ssize_t n = read(*(int *)ctx, buf, len); if (n > 0) return (int)n; if (n < 0 && errno == EINTR) continue; return -1; }
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    for (;;) { ssize_t n = send(*(int *)ctx, buf, len, MSG_NOSIGNAL); if (n > 0) return (int)n; if (n < 0 && errno == EINTR) continue; return -1; }
}

static void conn_close(struct conn *c) {
    if (c->tls && c->sc) br_sslio_close(&c->io);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    free(c->sc); free(c->xm); free(c->pin); free(c->iobuf);
    c->sc = NULL; c->xm = NULL; c->pin = NULL; c->iobuf = NULL;
}

static int conn_open(struct conn *c) {
    memset(c, 0, sizeof *c);
    pthread_mutex_lock(&rlock);
    struct route r = cur_route;
    pthread_mutex_unlock(&rlock);
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0) return -1;
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(r.wifi ? r.port : AGENT_PORT)),
                             .sin_addr.s_addr = r.wifi ? htonl(r.ip) : htonl(INADDR_LOOPBACK)};
    struct timeval tv = {15, 0}, quick = {4, 0};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, r.wifi ? &quick : &tv, sizeof tv); /* connect() honours SO_SNDTIMEO */
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(c->fd); c->fd = -1; return -1; }
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (!r.wifi) return 0;
    c->tls = 1;
    snprintf(c->auth, sizeof c->auth, "Authorization: Bearer %s\r\n", r.token);
    c->sc = calloc(1, sizeof *c->sc); c->xm = calloc(1, sizeof *c->xm); c->pin = calloc(1, sizeof *c->pin);
    c->iobuf = malloc(BR_SSL_BUFSIZE_BIDI);
    if (!c->sc || !c->xm || !c->pin || !c->iobuf) { conn_close(c); return -1; }
    br_ssl_client_init_full(c->sc, c->xm, NULL, 0);
    c->pin->vtable = &pin_vtable;
    memcpy(c->pin->want, r.fp, 32);
    br_ssl_engine_set_x509(&c->sc->eng, &c->pin->vtable);
    br_ssl_engine_set_buffer(&c->sc->eng, c->iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    if (!br_ssl_client_reset(c->sc, NULL, 0)) { conn_close(c); return -1; }
    br_sslio_init(&c->io, &c->sc->eng, sock_read, &c->fd, sock_write, &c->fd);
    return 0;
}

static int conn_send(struct conn *c, const char *b, size_t n) {
    if (!c->tls) return send(c->fd, b, n, MSG_NOSIGNAL) == (ssize_t)n ? 0 : -1;
    if (br_sslio_write_all(&c->io, b, n) < 0 || br_sslio_flush(&c->io) < 0) {
        logmsg("phonebox: TLS error %d", br_ssl_engine_last_error(&c->sc->eng));
        return -1;
    }
    return 0;
}

static int raw_getc(struct conn *c) {
    if (c->pos >= c->len) {
        int n = c->tls ? br_sslio_read(&c->io, c->buf, sizeof c->buf) : (int)recv(c->fd, c->buf, sizeof c->buf, 0);
        if (n <= 0) return -1;
        c->len = n; c->pos = 0;
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

static int body_getc(struct conn *c) {
    if (c->chunked) {
        if (c->remain == 0) {
            char l[32];
            if (raw_line(c, l, sizeof l) < 0) return -1;
            if (!l[0] && raw_line(c, l, sizeof l) < 0) return -1;
            c->remain = strtol(l, NULL, 16);
            if (c->remain <= 0) return -1;
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
    char rq[1024];
    int n = snprintf(rq, sizeof rq,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-PhoneBox-App: %s\r\nX-PhoneBox-Device: phonekit0001\r\n"
                     "X-PhoneBox-Model: PhoneKit\r\n%sAccept: */*\r\nConnection: close\r\n%s\r\n",
                     method, path, AGENT_PORT, APP_VERSION, c->auth, !strcmp(method, "POST") ? "Content-Length: 0\r\n" : "");
    if (n >= (int)sizeof rq || conn_send(c, rq, (size_t)n) < 0) { conn_close(c); return -1; }
    char l[512];
    if (raw_line(c, l, sizeof l) < 0) { conn_close(c); return -1; }
    int code = 0;
    sscanf(l, "HTTP/%*s %d", &code);
    c->remain = -1;
    while (raw_line(c, l, sizeof l) > 0) {
        if (!strncasecmp(l, "transfer-encoding:", 18) && strcasestr(l, "chunked")) { c->chunked = 1; c->remain = 0; }
        else if (!strncasecmp(l, "content-length:", 15)) c->remain = atol(l + 15);
    }
    return code;
}

static struct blob read_body(struct conn *c, size_t max) {
    struct blob b = {malloc(16384), 0};
    size_t cap = 16384;
    int ch;
    while (b.data && (ch = body_getc(c)) >= 0 && b.len < max) {
        if (b.len + 1 >= cap) { cap *= 2; char *nb = realloc(b.data, cap); if (!nb) { free(b.data); b.data = NULL; break; } b.data = nb; }
        b.data[b.len++] = (char)ch;
    }
    if (b.data) b.data[b.len] = 0;
    return b;
}

static struct blob get(const char *path, size_t max) {
    struct conn c;
    struct blob b = {0};
    if (request(&c, "GET", path) == 200) b = read_body(&c, max);
    conn_close(&c);
    return b;
}

static void publish(struct blob *slot, struct blob b) {
    pthread_mutex_lock(&lock);
    free(slot->data);
    *slot = b;
    pthread_mutex_unlock(&lock);
    char x = 1;
    if (notify_w >= 0) (void)!write(notify_w, &x, 1);
}

static long long json_ll_after(const char *s, const char *key) {
    const char *p = strstr(s, key);
    return p ? atoll(p + strlen(key)) : -1;
}

/* Side fetches (art, weather) on short-lived connections, next to the stream. */
static void side_fetches(void) {
    static long art_have = -1;
    long a = art_want;
    if (a != art_have) {
        art_have = a;
        char p[64];
        snprintf(p, sizeof p, "/api/art?id=%ld", a);
        struct blob b = a > 0 ? get(p, 4 << 20) : (struct blob){0};
        publish(&in_art, b.data ? b : (struct blob){calloc(1, 1), 0});
    }
    if (wx_want && wx_query[0]) {
        wx_want = 0;
        char p[1024], url[512];
        snprintf(url, sizeof url,
                 "https://api.open-meteo.com/v1/forecast?%s&current=temperature_2m,apparent_temperature,weather_code,is_day"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=7", wx_query);
        size_t o = (size_t)snprintf(p, sizeof p, "/api/proxy?url=");
        for (const char *u = url; *u && o + 4 < sizeof p; u++) {
            unsigned char ch = (unsigned char)*u;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) p[o++] = (char)ch;
            else o += (size_t)snprintf(p + o, sizeof p - o, "%%%02X", ch);
        }
        p[o] = 0;
        struct blob b = get(p, 1 << 20);
        if (b.data) publish(&in_wx, b);
    }
}

static void *side_thread(void *v) {
    (void)v;
    long q_at = -600000, p_at = 0;
    for (;;) {
        if (connected) {
            side_fetches();
            if (want_quotas && now_ms() - q_at > 60000) {
                q_at = now_ms();
                struct blob b = get("/api/quotas", 1 << 20);
                if (b.data && strstr(b.data, "\"providers\":[]")) q_at -= 50000; /* agent still collecting: retry in 10 s */
                if (b.data) publish(&in_quota, b);
            }
            if (want_approvals) { want_approvals = 0; struct blob b = get("/api/approvals", 1 << 20); if (b.data) publish(&in_appr, b); }
            if (want_procs && now_ms() - p_at > 3000) { p_at = now_ms(); struct blob b = get("/api/procs?top=12", 1 << 20); if (b.data) publish(&in_procs, b); }
        }
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- pairing (Wi-Fi route) */

static int hexbyte(const char *h) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = h[i];
        v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1000);
    }
    return v;
}

/* Over USB: fetch /api/pair and remember how to reach this PC over Wi-Fi (the agent's "Wi-Fi & Tailscale"
 * setting must be on). Stored in the settings store; the token is masked in dumps. */
static void pair_over_usb(void) {
    struct blob b = get("/api/pair", 64 * 1024);
    if (!b.data) return;
    static jtok t[512];
    int n = json_parse(b.data, (int)b.len, t, 512);
    char en[8] = "", tok[96] = "", fp[80] = "", hosts[256] = "", port[8] = "";
    if (n > 0) {
        json_str(b.data, t, json_get(b.data, t, n, 0, "enabled"), en, sizeof en);
        json_str(b.data, t, json_get(b.data, t, n, 0, "token"), tok, sizeof tok);
        json_str(b.data, t, json_get(b.data, t, n, 0, "fingerprint"), fp, sizeof fp);
        json_str(b.data, t, json_get(b.data, t, n, 0, "port"), port, sizeof port);
        int h = json_get(b.data, t, n, 0, "hosts");
        size_t o = 0;
        if (h >= 0 && t[h].type == J_ARR)
            for (int i = 0, q = h + 1; i < t[h].size && o + 20 < sizeof hosts; i++, q = json_skip(t, n, q)) {
                char ip[24];
                json_str(b.data, t, q, ip, sizeof ip);
                o += (size_t)snprintf(hosts + o, sizeof hosts - o, "%s%s", o ? "," : "", ip);
            }
    }
    free(b.data);
    if (strcmp(en, "true") || strlen(fp) != 64 || !tok[0] || !hosts[0]) return; /* Wi-Fi off on the PC: keep what we had */
    if (strcmp(store_get("pb.fp", ""), fp) || strcmp(store_get("pb.hosts", ""), hosts) || strcmp(store_get("pb.token", ""), tok)) {
        store_set("pb.fp", fp); store_set("pb.hosts", hosts); store_set("pb.port", port); store_set("pb.token", tok);
        logmsg("phonebox: paired for Wi-Fi (%s, port %s)", hosts, port);
    }
}

/* Picks the route for the next connection: USB when the tunnel is up (unless pb.route=wifi forces Wi-Fi for
 * tests), else the paired PC over Wi-Fi, trying its addresses in turn. Returns 0 if there is no route. */
static int pick_route(int attempt) {
    struct route r = {0};
    int usb = adbd_reverse_active(AGENT_PORT);
    int force = !strcmp(store_get("pb.route", ""), "wifi");
    const char *fp = store_get("pb.fp", ""), *hosts = store_get("pb.hosts", ""), *tok = store_get("pb.token", "");
    if (usb && !force) r.wifi = 0;
    else if (strlen(fp) == 64 && hosts[0] && tok[0]) {
        r.wifi = 1;
        r.port = atoi(store_get("pb.port", "8766"));
        snprintf(r.token, sizeof r.token, "%s", tok);
        for (int i = 0; i < 32; i++) r.fp[i] = (unsigned char)hexbyte(fp + 2 * i);
        /* addresses: "a,b,c" -> pick attempt % count */
        char hs[256];
        snprintf(hs, sizeof hs, "%s", hosts);
        int cnt = 0;
        uint32_t list[8];
        /* the PC lists every address it has (LAN, Docker bridges, our own USB link). Skip the USB link (10.42.0.0/24,
         * never right over Wi-Fi) and put the ones on the phone's Wi-Fi subnet first: a dead address costs a
         * connect timeout, which made the switch to Wi-Fi take ~40 s */
        char wip[32] = "";
        struct in_addr me = {0};
        if (read_file("/run/wifi.ip", wip, sizeof wip) > 0) { wip[strcspn(wip, "\n")] = 0; inet_aton(wip, &me); }
        uint32_t mine = ntohl(me.s_addr);
        char *save = NULL;
        for (int pass = 0; pass < 2; pass++) {
            snprintf(hs, sizeof hs, "%s", hosts);
            save = NULL;
            for (char *h = strtok_r(hs, ",", &save); h && cnt < 8; h = strtok_r(NULL, ",", &save)) {
                struct in_addr a;
                if (!inet_aton(h, &a)) continue;
                uint32_t ip = ntohl(a.s_addr);
                if ((ip & 0xffffff00u) == 0x0a2a0000u) continue; /* 10.42.0.x: the USB network */
                int same = mine && (ip & 0xffffff00u) == (mine & 0xffffff00u);
                if (same == !pass) list[cnt++] = ip; /* pass 0: same subnet, pass 1: the rest */
            }
        }
        if (!cnt) return 0;
        r.ip = list[attempt % cnt];
    } else return 0;
    pthread_mutex_lock(&rlock);
    cur_route = r;
    pthread_mutex_unlock(&rlock);
    route_wifi = r.wifi;
    return 1;
}

static void *http_thread(void *v) {
    (void)v;
    char *line = malloc(256 * 1024);
    int attempt = 0;
    for (;;) {
        if (!pick_route(attempt)) { connected = 0; sleep(1); continue; }
        if (!route_wifi) pair_over_usb();
        struct blob cfg = get("/api/config", 4 << 20);
        if (!cfg.data) { connected = 0; attempt++; sleep(route_wifi ? 3 : 2); continue; }
        cfg_version = json_ll_after(cfg.data, "\"version\":");
        n_cfgs++;
        publish(&in_cfg, cfg);
        struct conn c;
        if (request(&c, "GET", "/api/stream") != 200) { conn_close(&c); sleep(2); continue; }
        connected = 1;
        attempt = 0; /* next time, start again with the best address */
        logmsg("phonebox: live stream open over %s (config v%lld)", route_wifi ? "Wi-Fi (TLS, pinned)" : "USB", cfg_version);
        char ev[32] = "message";
        long opened = now_ms();
        stream_opened_ms = opened;
        if (route_wifi) wifi_stream_fd = c.fd;
        for (;;) {
            if (route_wifi && adbd_reverse_active(AGENT_PORT) && strcmp(store_get("pb.route", ""), "wifi") && now_ms() - opened > 3000) break; /* cable back: USB wins */
            int n = body_line(&c, line, 256 * 1024);
            if (n < 0) break;
            if (!strncmp(line, "event: ", 7)) snprintf(ev, sizeof ev, "%s", line + 7);
            else if (!strncmp(line, "data: ", 6) && !strcmp(ev, "work")) publish(&in_work, (struct blob){strdup(line + 6), (size_t)(n - 6)});
            else if (!strncmp(line, "data: ", 6) && !strcmp(ev, "event")) {
                pthread_mutex_lock(&lock);
                if (evn == EVQ) { free(evq[0]); memmove(evq, evq + 1, sizeof evq[0] * (EVQ - 1)); evn--; }
                evq[evn++] = strdup(line + 6);
                pthread_mutex_unlock(&lock);
                char x = 1;
                (void)!write(notify_w, &x, 1);
                if (strstr(line, "\"kind\":\"refresh\"")) break; /* refetch the config */
            }
            else if (!strncmp(line, "data: ", 6) && !strcmp(ev, "state")) {
                n_states++;
                long long ver = json_ll_after(line, "\"cfgVersion\":");
                publish(&in_state, (struct blob){strdup(line + 6), (size_t)(n - 6)});
                if (ver > 0 && ver != cfg_version) break; /* layout edited on the PC: refetch */
            } else if (!n) snprintf(ev, sizeof ev, "message");
        }
        wifi_stream_fd = -1;
        conn_close(&c);
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
    conn_close(&c);
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

/* ================================================================ JSON helpers (main loop) */

#define MAXTOK 16384
static jtok tk[MAXTOK];

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
static void str(const char *s, int n, int o, const char *path, char *out, size_t len) {
    out[0] = 0;
    int i = jpath(s, n, o, path);
    if (!is_null(s, i)) json_str(s, tk, i, out, len);
}

/* ================================================================ model */

enum { T_CLOCK, T_STAT, T_GAUGE, T_CHART, T_METERS, T_MEDIA, T_SHORTCUTS, T_NOTE, T_FORECAST, T_QUOTAS, T_SESSIONS, T_GITHUB, T_PROCS, T_OTHER };
enum { M_CPU, M_GPU, M_RAM, M_VRAM, M_NET, M_DOWN, M_UP, M_PING, M_GPUTEMP, M_CPUTEMP, M_N };
struct pbw {
    int type, x, y, w, h;
    int metric[2], nmetric;
    int status, weather, sc_cols;
    char text[160], tname[24];
};
#define PB_PAGES 8
#define PB_W 24
static struct { char id[32], title[32]; int n; struct pbw w[PB_W]; } pg[PB_PAGES];
static int npg, cur, have_cfg;
static struct { char id[32], label[40]; uint32_t color; } sc[32];
static int nsc;

#define HIST 60
static float hist[M_N][HIST];
static int hn, hpos;
static struct {
    double cpu, cputemp, gpu, gputemp, gpupower, ram, ramt, vram, vramt, down, up, ping, uptime;
    int has_gpu, ndisk;
    char host[40], cpuname[64], gpuname[64];
    struct { char name[16]; double pct; } disk[4];
    int media, playing;
    char app[32], title[96], artist[96];
    long art, pos_ms, dur_ms, at_ms;
    double vol;
    int has_vol, muted;
} S;
static struct canvas art;
static long art_id_loaded = -1;      /* artId decoded into `art` */
static struct blob art_raw;          /* the JPEG/PNG bytes, re-decoded when the drawn size changes */
static long art_raw_id = -1;
static struct { int ok, code, is_day; double t, feels, tmax, tmin; char city[40]; int dcode[7]; double dmax[7], dmin[7]; int nd; } W;
/* list widgets: quotas, sessions, github, processes */
struct row { char a[64], b[48], c[24], err[96]; double pct; uint32_t col; };
#define ROWS 12
static struct row q_rows[ROWS], s_rows[ROWS], g_rows[ROWS], p_rows[ROWS];
static int nq, ns, ng, np, g_notif, p_total;
static char g_err[120], q_head[40];
static long tz_off;            /* seconds east of UTC, from open-meteo for the configured place */
static long long pc_ts_skew;   /* PC epoch ms − phone epoch ms */

/* standby (Settings > WHEN PC IS GONE): hold the last page, an ambient clock, or the screen off */
static int gone;                 /* currently in standby */
static long last_seen_ms;        /* last time the agent stream was up */

static void draw_ambient(void);
static const char *standby_mode(void);
int pb_agent_muted;
static void draw_overlays(void);
static int modal(void);
static void draw_toast(void);
static struct { char title[64], text[160]; uint32_t col; long until; } toast;

static struct fb *fb;
static struct canvas cv;
static float SC;  /* device px per CSS px */
static int land;

/* colours (phonebox style.css :root) */
#define BG 0x06080b
#define TEXT 0xeef2f6
#define MUTED 0x8a94a3
#define DIM 0x5b6472
static const uint32_t MCOL[M_N] = {0x2dd4bf, 0x84cc16, 0xa78bfa, 0x84cc16, 0xf59e0b, 0xf59e0b, 0x60a5fa, 0x38bdf8, 0xf97316, 0xf97316};
static const char *const MLABEL[M_N] = {"CPU", "GPU", "RAM", "VRAM", "NETWORK", "DOWNLOAD", "UPLOAD", "PING", "GPU TEMP", "CPU TEMP"};
static const char *const MNAME[M_N] = {"Processor", "Graphics", "Memory", "Video memory", "Network", "Download", "Upload", "Ping", "GPU temperature", "CPU temperature"};

static int metric_of(const char *m) {
    static const char *const K[M_N] = {"cpu", "gpu", "ram", "vram", "net", "down", "up", "ping", "gputemp", "cputemp"};
    for (int i = 0; i < M_N; i++) if (!strcmp(K[i], m)) return i;
    return -1;
}

static uint32_t parse_hex(const char *c, uint32_t d) {
    if (c[0] != '#' || strlen(c) < 7) return d;
    return (uint32_t)strtoul(c + 1, NULL, 16) & 0xffffff;
}

/* ================================================================ config → model */

static void load_config(const char *cfgs) {
    int n = json_parse(cfgs, (int)strlen(cfgs), tk, MAXTOK);
    if (n < 1) { logmsg("phonebox: config JSON unparsable (%d)", n); return; }
    int c = json_get(cfgs, tk, n, 0, "config");
    int pages = jpath(cfgs, n, c, "pages"), shs = jpath(cfgs, n, c, "shortcuts");
    nsc = 0;
    if (shs >= 0 && tk[shs].type == J_ARR) {
        int j = shs + 1;
        for (int k = 0; k < tk[shs].size && nsc < 32; k++, j = json_skip(tk, n, j)) {
            char col[16];
            str(cfgs, n, j, "id", sc[nsc].id, sizeof sc[nsc].id);
            str(cfgs, n, j, "label", sc[nsc].label, sizeof sc[nsc].label);
            str(cfgs, n, j, "color", col, sizeof col);
            sc[nsc].color = parse_hex(col, 0x64748b);
            nsc++;
        }
    }
    double lat = num(cfgs, n, c, "weather.lat", 1000), lon = num(cfgs, n, c, "weather.lon", 1000);
    str(cfgs, n, c, "weather.city", W.city, sizeof W.city);
    if (lat < 1000 && lon < 1000) { snprintf(wx_query, sizeof wx_query, "latitude=%.4f&longitude=%.4f", lat, lon); wx_want = 1; }
    npg = 0;
    if (pages < 0 || tk[pages].type != J_ARR) return;
    int j = pages + 1;
    for (int k = 0; k < tk[pages].size && npg < PB_PAGES; k++, j = json_skip(tk, n, j)) {
        if (tk[j].type != J_OBJ) continue;
        int en = json_get(cfgs, tk, n, j, "enabled");
        if (en >= 0 && cfgs[tk[en].start] == 'f') continue;
        struct pbw *ws = pg[npg].w;
        pg[npg].n = 0;
        str(cfgs, n, j, "title", pg[npg].title, sizeof pg[npg].title);
        str(cfgs, n, j, "id", pg[npg].id, sizeof pg[npg].id);
        int arr = json_get(cfgs, tk, n, j, "widgets");
        if (arr >= 0 && tk[arr].type == J_ARR) {
            int q = arr + 1;
            for (int i = 0; i < tk[arr].size && pg[npg].n < PB_W; i++, q = json_skip(tk, n, q)) {
                struct pbw *w = &ws[pg[npg].n];
                memset(w, 0, sizeof *w);
                char type[24];
                str(cfgs, n, q, "type", type, sizeof type);
                snprintf(w->tname, sizeof w->tname, "%s", type);
                w->x = (int)num(cfgs, n, q, "x", 0); w->y = (int)num(cfgs, n, q, "y", 0);
                w->w = (int)num(cfgs, n, q, "w", 1); w->h = (int)num(cfgs, n, q, "h", 1);
                if (w->x < 0 || w->y < 0 || w->w < 1 || w->h < 1 || w->x + w->w > 12 || w->y + w->h > 6) continue;
                int opt = json_get(cfgs, tk, n, q, "options");
                char m[16];
                if (!strcmp(type, "clock")) {
                    w->type = T_CLOCK;
                    int st = jpath(cfgs, n, opt, "status"), wx = jpath(cfgs, n, opt, "weather");
                    w->status = st < 0 || cfgs[tk[st].start] != 'f';
                    w->weather = wx < 0 || cfgs[tk[wx].start] != 'f';
                } else if (!strcmp(type, "stat") || !strcmp(type, "gauge")) {
                    w->type = !strcmp(type, "stat") ? T_STAT : T_GAUGE;
                    str(cfgs, n, opt, "metric", m, sizeof m);
                    w->metric[0] = metric_of(m) < 0 ? M_CPU : metric_of(m);
                    w->nmetric = 1;
                } else if (!strcmp(type, "chart")) {
                    w->type = T_CHART;
                    int ar = jpath(cfgs, n, opt, "metrics");
                    if (ar >= 0 && tk[ar].type == J_ARR)
                        for (int z = 0, e = ar + 1; z < tk[ar].size && w->nmetric < 2; z++, e = json_skip(tk, n, e)) {
                            json_str(cfgs, tk, e, m, sizeof m);
                            if (metric_of(m) >= 0) w->metric[w->nmetric++] = metric_of(m);
                        }
                    if (!w->nmetric) { w->metric[0] = M_CPU; w->nmetric = 1; }
                } else if (!strcmp(type, "meters")) w->type = T_METERS;
                else if (!strcmp(type, "media")) w->type = T_MEDIA;
                else if (!strcmp(type, "shortcuts")) { w->type = T_SHORTCUTS; w->sc_cols = (int)num(cfgs, n, opt, "columns", 4); }
                else if (!strcmp(type, "note") || !strcmp(type, "label") || !strcmp(type, "quote")) {
                    w->type = T_NOTE;
                    str(cfgs, n, opt, "text", w->text, sizeof w->text);
                } else if (!strcmp(type, "forecast") || !strcmp(type, "weather")) w->type = T_FORECAST;
                else if (!strcmp(type, "quotas") || !strcmp(type, "quotaring")) { w->type = T_QUOTAS; want_quotas = 1; }
                else if (!strcmp(type, "sessions")) w->type = T_SESSIONS;
                else if (!strcmp(type, "github")) w->type = T_GITHUB;
                else if (!strcmp(type, "processes") || !strcmp(type, "topprocs")) w->type = T_PROCS;
                else w->type = T_OTHER;
                pg[npg].n++;
            }
        }
        npg++;
    }
    if (cur >= npg) cur = 0;
    have_cfg = npg > 0;
    want_approvals = 1;
    logmsg("phonebox: %d pages, %d shortcuts", npg, nsc);
}

/* ================================================================ state → model */

static void load_state(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    if (n < 1) return;
    long long ts = (long long)num(s, n, 0, "ts", 0);
    if (ts > 0) { struct timespec now; clock_gettime(CLOCK_REALTIME, &now); pc_ts_skew = ts - (long long)now.tv_sec * 1000 - now.tv_nsec / 1000000; }
    str(s, n, 0, "host", S.host, sizeof S.host);
    S.cpu = num(s, n, 0, "pc.cpu", 0);
    S.cputemp = num(s, n, 0, "pc.cpuTemp", -1);
    str(s, n, 0, "pc.cpuName", S.cpuname, sizeof S.cpuname);
    int g = jpath(s, n, 0, "pc.gpu");
    S.has_gpu = !is_null(s, g);
    S.gpu = num(s, n, g, "util", 0); S.gputemp = num(s, n, g, "temp", -1); S.gpupower = num(s, n, g, "power", -1);
    S.vram = num(s, n, g, "vramUsed", 0); S.vramt = num(s, n, g, "vramTotal", 0);
    str(s, n, g, "name", S.gpuname, sizeof S.gpuname);
    S.ram = num(s, n, 0, "pc.ramUsedGb", 0); S.ramt = num(s, n, 0, "pc.ramTotalGb", 1);
    S.down = num(s, n, 0, "pc.netDownBps", 0); S.up = num(s, n, 0, "pc.netUpBps", 0);
    S.ping = num(s, n, 0, "ping.ms", -1);
    S.uptime = num(s, n, 0, "pc.uptimeSec", 0);
    int d = jpath(s, n, 0, "pc.disks");
    S.ndisk = 0;
    if (d >= 0 && tk[d].type == J_ARR)
        for (int i = 0, q = d + 1; i < tk[d].size && S.ndisk < 4; i++, q = json_skip(tk, n, q)) {
            double u = num(s, n, q, "usedGb", 0), t = num(s, n, q, "totalGb", 0);
            if (t <= 0) continue;
            str(s, n, q, "name", S.disk[S.ndisk].name, sizeof S.disk[S.ndisk].name);
            S.disk[S.ndisk].pct = u / t * 100;
            S.ndisk++;
        }
    int m = json_get(s, tk, n, 0, "media");
    S.media = !is_null(s, m);
    if (S.media) {
        str(s, n, m, "app", S.app, sizeof S.app);
        str(s, n, m, "title", S.title, sizeof S.title);
        str(s, n, m, "artist", S.artist, sizeof S.artist);
        char album[64];
        str(s, n, m, "album", album, sizeof album);
        if (album[0]) { size_t l = strlen(S.artist); snprintf(S.artist + l, sizeof S.artist - l, "%s%s", l ? " \xc2\xb7 " : "", album); }
        S.playing = num(s, n, m, "playing", 0) > 0;
        S.art = (long)num(s, n, m, "artId", 0);
        S.pos_ms = (long)num(s, n, m, "posMs", 0); S.dur_ms = (long)num(s, n, m, "durMs", 0);
        S.at_ms = now_ms();
    } else { S.playing = 0; S.art = 0; }
    art_want = S.art;
    int a = json_get(s, tk, n, 0, "audio");
    S.has_vol = !is_null(s, a);
    S.vol = num(s, n, a, "volume", 0);
    int mu = jpath(s, n, a, "muted");
    S.muted = mu >= 0 && s[tk[mu].start] == 't';
    float v[M_N] = {(float)S.cpu, (float)S.gpu, (float)S.ram, S.vramt > 0 ? (float)(S.vram / S.vramt * 100) : 0,
                    (float)S.down, (float)S.down, (float)S.up, (float)(S.ping < 0 ? 0 : S.ping),
                    (float)(S.gputemp < 0 ? 0 : S.gputemp), (float)(S.cputemp < 0 ? 0 : S.cputemp)};
    for (int i = 0; i < M_N; i++) hist[i][hpos] = v[i];
    hpos = (hpos + 1) % HIST;
    if (hn < HIST) hn++;
}

static void load_weather(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    if (n < 1 || json_get(s, tk, n, 0, "current") < 0) return;
    tz_off = (long)num(s, n, 0, "utc_offset_seconds", 0);
    W.t = num(s, n, 0, "current.temperature_2m", 0);
    W.feels = num(s, n, 0, "current.apparent_temperature", 0);
    W.code = (int)num(s, n, 0, "current.weather_code", 0);
    W.is_day = num(s, n, 0, "current.is_day", 1) > 0;
    int dc = jpath(s, n, 0, "daily.weather_code"), mx = jpath(s, n, 0, "daily.temperature_2m_max"), mn = jpath(s, n, 0, "daily.temperature_2m_min");
    W.nd = 0;
    if (dc >= 0 && mx >= 0 && mn >= 0 && tk[dc].type == J_ARR)
        for (int i = 0; i < tk[dc].size && i < 7; i++) {
            W.dcode[i] = (int)json_num(s, tk, dc + 1 + i, 0);
            W.dmax[i] = json_num(s, tk, mx + 1 + i, 0);
            W.dmin[i] = json_num(s, tk, mn + 1 + i, 0);
            W.nd++;
        }
    if (W.nd) { W.tmax = W.dmax[0]; W.tmin = W.dmin[0]; }
    W.ok = 1;
}

static void dur_text(long sec, char *o, size_t l) {
    if (sec < 0) sec = 0;
    if (sec >= 86400) snprintf(o, l, "%ldd %ldh", sec / 86400, sec % 86400 / 3600);
    else if (sec >= 3600) snprintf(o, l, "%ldh %ldm", sec / 3600, sec % 3600 / 60);
    else snprintf(o, l, "%ldm", sec / 60);
}

static void load_quotas(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    int pr = n > 0 ? json_get(s, tk, n, 0, "providers") : -1;
    nq = 0;
    if (pr < 0 || tk[pr].type != J_ARR) return;
    long now = (long)(time(NULL) + pc_ts_skew / 1000);
    for (int i = 0, p = pr + 1; i < tk[pr].size; i++, p = json_skip(tk, n, p)) {
        char name[32];
        str(s, n, p, "name", name, sizeof name);
        int lim = json_get(s, tk, n, p, "limits");
        char perr[96];
        str(s, n, p, "error", perr, sizeof perr);
        if ((lim < 0 || tk[lim].type != J_ARR || tk[lim].size == 0) && nq < ROWS) { /* no numbers: say why, as the app does */
            struct row *r = &q_rows[nq++];
            snprintf(r->a, sizeof r->a, "%.30s", name);
            snprintf(r->b, sizeof r->b, "\xe2\x80\x94");
            snprintf(r->c, sizeof r->c, "%.23s", perr[0] ? perr : "no data");
            snprintf(r->err, sizeof r->err, "%s", perr[0] ? perr : "No data yet");
            r->pct = -1;
            r->col = 0x8a94a3;
            continue;
        }
        for (int k = 0, l = lim + 1; k < tk[lim].size && nq < ROWS; k++, l = json_skip(tk, n, l)) {
            struct row *r = &q_rows[nq++];
            char lbl[48], sev[16];
            str(s, n, l, "label", lbl, sizeof lbl);
            str(s, n, l, "severity", sev, sizeof sev);
            snprintf(r->a, sizeof r->a, "%.20s \xc2\xb7 %.36s", name, lbl);
            r->pct = num(s, n, l, "percent", 0);
            snprintf(r->b, sizeof r->b, "%.0f%%", r->pct);
            long rs = (long)(num(s, n, l, "resetsAt", 0) / 1000);
            char d[24];
            dur_text(rs - now, d, sizeof d);
            if (rs > 0) snprintf(r->c, sizeof r->c, "resets in %s", d); else r->c[0] = 0;
            r->col = !strcmp(sev, "critical") ? 0xef4444 : !strcmp(sev, "warning") ? 0xf59e0b : 0x22c55e;
        }
    }
    snprintf(q_head, sizeof q_head, "AI QUOTAS");
}

static void load_work(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    if (n < 1) return;
    int ss = json_get(s, tk, n, 0, "sessions");
    ns = 0;
    if (ss >= 0 && tk[ss].type == J_ARR)
        for (int i = 0, q = ss + 1; i < tk[ss].size && ns < ROWS; i++, q = json_skip(tk, n, q)) {
            struct row *r = &s_rows[ns++];
            char tool[24], name[48], st[24], wait[32];
            str(s, n, q, "tool", tool, sizeof tool); str(s, n, q, "name", name, sizeof name);
            str(s, n, q, "project", r->b, sizeof r->b); str(s, n, q, "status", st, sizeof st);
            str(s, n, q, "waitingFor", wait, sizeof wait);
            snprintf(r->a, sizeof r->a, "%.20s \xc2\xb7 %.40s", tool, name[0] ? name : "session");
            snprintf(r->c, sizeof r->c, "%s", wait[0] ? "waiting" : st);
            r->col = wait[0] ? 0xf59e0b : !strcmp(st, "running") || !strcmp(st, "working") ? 0x22c55e : 0x8a94a3;
        }
    int g = json_get(s, tk, n, 0, "github");
    ng = 0; g_err[0] = 0;
    if (g < 0) return;
    str(s, n, g, "error", g_err, sizeof g_err);
    g_notif = (int)num(s, n, g, "notifications", 0);
    const char *lists[2] = {"reviewRequests", "myPrs"};
    for (int L = 0; L < 2; L++) {
        int a = json_get(s, tk, n, g, lists[L]);
        if (a < 0 || tk[a].type != J_ARR) continue;
        for (int i = 0, q = a + 1; i < tk[a].size && ng < ROWS; i++, q = json_skip(tk, n, q)) {
            struct row *r = &g_rows[ng++];
            char title[64], repo[40], ci[16];
            str(s, n, q, "title", title, sizeof title); str(s, n, q, "repo", repo, sizeof repo); str(s, n, q, "ci", ci, sizeof ci);
            snprintf(r->a, sizeof r->a, "#%.0f %.54s", num(s, n, q, "number", 0), title);
            snprintf(r->b, sizeof r->b, "%.30s%s", repo, L == 0 ? " \xc2\xb7 review" : "");
            snprintf(r->c, sizeof r->c, "%s", ci);
            r->col = !strcmp(ci, "failure") ? 0xef4444 : !strcmp(ci, "success") ? 0x22c55e : 0x8a94a3;
        }
    }
}

static void load_procs(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    if (n < 1) return;
    p_total = (int)num(s, n, 0, "total", 0);
    int g = json_get(s, tk, n, 0, "groups");
    np = 0;
    if (g < 0 || tk[g].type != J_ARR) return;
    for (int i = 0, q = g + 1; i < tk[g].size && np < ROWS; i++, q = json_skip(tk, n, q)) {
        struct row *r = &p_rows[np++];
        char nm[48], title[48];
        str(s, n, q, "name", nm, sizeof nm); str(s, n, q, "title", title, sizeof title);
        int cnt = (int)num(s, n, q, "count", 1);
        snprintf(r->a, sizeof r->a, cnt > 1 ? "%.40s (%d)" : "%.40s", title[0] ? title : nm, cnt);
        r->pct = num(s, n, q, "cpu", 0);
        snprintf(r->b, sizeof r->b, "%.1f%%", r->pct);
        double mb = num(s, n, q, "memMb", 0);
        if (mb >= 1024) snprintf(r->c, sizeof r->c, "%.1f GB", mb / 1024); else snprintf(r->c, sizeof r->c, "%.0f MB", mb);
        r->col = 0x2dd4bf;
    }
}

/* ================================================================ drawing helpers */

static int P(float css) { return (int)lroundf(css * SC); }
static float clampf(float lo, float v, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void grid_rect(const struct pbw *w, int *X, int *Y, int *Wd, int *Ht) {
    int gx = w->x, gy = w->y, gw = w->w, gh = w->h, cols = 12, rows = 6;
    if (!land) { gx = w->y; gy = w->x; gw = w->h; gh = w->w; cols = 6; rows = 12; }
    float pl = 14 * SC, pr = 14 * SC, pt = 10 * SC, pb = 17 * SC, gap = 8 * SC;
    float cw = (fb->w - pl - pr - gap * (cols - 1)) / cols, ch = (fb->h - pt - pb - gap * (rows - 1)) / rows;
    *X = (int)(pl + gx * (cw + gap)); *Y = (int)(pt + gy * (ch + gap));
    *Wd = (int)(gw * cw + (gw - 1) * gap); *Ht = (int)(gh * ch + (gh - 1) * gap);
}

static void card(int w, int h) {
    float m = fminf((float)w, (float)h) / SC;
    int r = P(clampf(10, 0.07f * m, 20));
    cv_rrect(&cv, 0, 0, w, h, r, 0xffffff, 11);                                /* --card: rgba(255,255,255,.045) */
    cv_rrect_stroke(&cv, 0, 0, w, h, r, fmaxf(1.5f, SC * 0.6f), 0xffffff, 20);  /* --line: .08 */
}

static void fmt_rate(double bps, char *v, size_t vl, char *u, size_t ul) {
    if (bps >= 1e9) { snprintf(v, vl, "%.1f", bps / 1e9); snprintf(u, ul, "GB/s"); }
    else if (bps >= 1e6) { snprintf(v, vl, "%.1f", bps / 1e6); snprintf(u, ul, "MB/s"); }
    else if (bps >= 1e3) { snprintf(v, vl, "%.0f", bps / 1e3); snprintf(u, ul, "KB/s"); }
    else { snprintf(v, vl, "%.0f", bps); snprintf(u, ul, "B/s"); }
}

/* phonebox metricText(): value + unit, plus the sub line */
static void metric_text(int m, char *v, size_t vl, char *u, size_t ul, char *sub, size_t sl) {
    u[0] = sub[0] = 0;
    switch (m) {
    case M_CPU:
        snprintf(v, vl, "%.0f", S.cpu); snprintf(u, ul, "%%");
        if (S.cputemp >= 0) snprintf(sub, sl, "%.0f\xc2\xb0" "C", S.cputemp); else snprintf(sub, sl, "%.30s", S.cpuname);
        break;
    case M_GPU:
        if (!S.has_gpu) { snprintf(v, vl, "\xe2\x80\x94"); snprintf(sub, sl, "No GPU data"); break; }
        snprintf(v, vl, "%.0f", S.gpu); snprintf(u, ul, "%%");
        if (S.gputemp >= 0 && S.gpupower >= 0) snprintf(sub, sl, "%.0f\xc2\xb0" "C \xc2\xb7 %.0f W", S.gputemp, S.gpupower);
        else if (S.gputemp >= 0) snprintf(sub, sl, "%.0f\xc2\xb0" "C", S.gputemp);
        break;
    case M_RAM: snprintf(v, vl, "%.1f", S.ram); snprintf(u, ul, "GB"); snprintf(sub, sl, "/ %.0f GB", S.ramt); break;
    case M_VRAM:
        snprintf(v, vl, "%.0f", S.vramt > 0 ? S.vram / S.vramt * 100 : 0); snprintf(u, ul, "%%");
        snprintf(sub, sl, "%.1f / %.0f GB", S.vram, S.vramt);
        break;
    case M_NET: case M_DOWN: case M_UP: {
        fmt_rate(m == M_UP ? S.up : S.down, v, vl, u, ul);
        if (m == M_NET) {
            char a[16], b[16];
            fmt_rate(S.up, a, sizeof a, b, sizeof b);
            if (S.ping >= 0) snprintf(sub, sl, "\xe2\x86\x91 %s %s \xc2\xb7 %.0f ms", a, b, S.ping);
            else snprintf(sub, sl, "\xe2\x86\x91 %s %s", a, b);
        }
        break;
    }
    case M_PING: if (S.ping >= 0) snprintf(v, vl, "%.0f", S.ping); else snprintf(v, vl, "\xe2\x80\x94"); snprintf(u, ul, "ms"); break;
    case M_GPUTEMP: snprintf(v, vl, "%.0f", S.gputemp < 0 ? 0 : S.gputemp); snprintf(u, ul, "\xc2\xb0" "C"); break;
    case M_CPUTEMP: snprintf(v, vl, "%.0f", S.cputemp < 0 ? 0 : S.cputemp); snprintf(u, ul, "\xc2\xb0" "C"); break;
    default: snprintf(v, vl, "?");
    }
}

static float metric_max(int m) {
    if (m == M_RAM) return (float)(S.ramt > 0 ? S.ramt : 1);
    if (m == M_NET || m == M_DOWN || m == M_UP) {
        float mx = 1e5f;
        for (int i = 0; i < HIST; i++) { if (hist[M_DOWN][i] > mx) mx = hist[M_DOWN][i]; if (hist[M_UP][i] > mx) mx = hist[M_UP][i]; }
        return mx * 1.2f;
    }
    if (m == M_PING) { float mx = 50; for (int i = 0; i < HIST; i++) if (hist[M_PING][i] * 1.2f > mx) mx = hist[M_PING][i] * 1.2f; return mx; }
    return 100;
}

static void spark(int m, int x, int y, int w, int h, int la, int fa) {
    int n = hn;
    if (n < 2) return;
    float mx = metric_max(m), v[HIST], full[HIST];
    for (int i = 0; i < n; i++) {
        float s = hist[m][(hpos - n + i + HIST) % HIST] / mx;
        v[i] = s < 0 ? 0 : s > 1 ? 1 : s;
    }
    for (int i = 0; i < HIST; i++) full[i] = i < HIST - n ? v[0] : v[i - (HIST - n)];
    cv_spark(&cv, x, y, w, h, full, HIST, MCOL[m], la, fa, fmaxf(2.0f, 1.5f * SC));
}

/* ---------------------------------------------------------------- widgets */

static void draw_stat(const struct pbw *wd, int w, int h) {
    card(w, h);
    float hc = h / SC, wc = w / SC;
    int m = wd->metric[0];
    int py = P(clampf(5, 0.09f * hc, 10)), px = P(clampf(8, 0.06f * wc, 12));
    int ls = P(clampf(9, 0.12f * hc, 11)), vs = P(clampf(16, 0.30f * hc, 40)), us = P(clampf(10, 0.14f * hc, 15)), ss = P(clampf(9.5f, 0.13f * hc, 13));
    spark(m, 0, h - (int)(h * 0.52f), w, (int)(h * 0.52f) - P(1), 128, 45);
    char v[24], u[12], sub[64], lbl[32];
    metric_text(m, v, sizeof v, u, sizeof u, sub, sizeof sub);
    if (m == M_RAM && S.ramt > 0) snprintf(lbl, sizeof lbl, "RAM (%.0f%%)", S.ram / S.ramt * 100);
    else snprintf(lbl, sizeof lbl, "%s", MLABEL[m]);
    int y = py + (int)(ls * 0.95f);
    cv_text(&cv, F_BOLD, ls, px, y, lbl, MCOL[m], 255, ls * 0.14f, w - 2 * px);
    y += vs;
    int x = cv_text(&cv, F_MEDIUM, vs, px, y, v, TEXT, 255, -vs * 0.02f, w - 2 * px);
    cv_text(&cv, F_REGULAR, us, x + P(2), y, u, MUTED, 255, 0, 0);
    y += (int)(ss * 1.35f);
    cv_text(&cv, F_REGULAR, ss, px, y, sub, MUTED, 255, 0, w - 2 * px);
}

static void draw_gauge(const struct pbw *wd, int w, int h) {
    card(w, h);
    int m = wd->metric[0];
    float r = fminf((float)w, (float)h) * 0.32f, cx = w / 2.0f, cy = h * 0.46f, th = fmaxf(4, r * 0.14f);
    float pct = hist[m][(hpos + HIST - 1) % HIST] / metric_max(m) * 100;
    pct = clampf(0, pct, 100);
    for (int i = 0; i < 120; i++) { /* track + value arc, from 135° sweeping 270° */
        float a0 = (135 + 270.0f * i / 120) * (float)M_PI / 180, a1 = (135 + 270.0f * (i + 1) / 120) * (float)M_PI / 180;
        int on = i < pct * 1.2f;
        cv_line(&cv, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), th, on ? MCOL[m] : 0xffffff, on ? 255 : 20);
    }
    char v[24], u[12], sub[64];
    metric_text(m, v, sizeof v, u, sizeof u, sub, sizeof sub);
    int vs = (int)(r * 0.55f), us = vs / 2;
    int tw = cv_text_w(F_MEDIUM, vs, v, 0) + cv_text_w(F_REGULAR, us, u, 0) + P(2);
    int x = cv_text(&cv, F_MEDIUM, vs, (int)(cx - tw / 2.0f), (int)(cy + vs * 0.35f), v, TEXT, 255, 0, 0);
    cv_text(&cv, F_REGULAR, us, x + P(2), (int)(cy + vs * 0.35f), u, MUTED, 255, 0, 0);
    int ls = P(11);
    int lw = cv_text_w(F_BOLD, ls, MLABEL[m], ls * 0.14f);
    cv_text(&cv, F_BOLD, ls, (int)(cx - lw / 2.0f), (int)(cy + r + ls * 1.3f), MLABEL[m], MCOL[m], 255, ls * 0.14f, 0);
}

static void draw_chart(const struct pbw *wd, int w, int h) {
    card(w, h);
    float hc = h / SC;
    int px = P(12), py = P(10), ls = P(11), bs = P(clampf(18, 0.15f * hc, 34)), ss = P(12.5f);
    int single = wd->nmetric == 1, net = 1;
    for (int i = 0; i < wd->nmetric; i++) if (wd->metric[i] != M_DOWN && wd->metric[i] != M_UP) net = 0;
    char lbl[48];
    if (single) snprintf(lbl, sizeof lbl, "%s", MNAME[wd->metric[0]]);
    else if (net) snprintf(lbl, sizeof lbl, "Network");
    else snprintf(lbl, sizeof lbl, "%s \xc2\xb7 %s", MNAME[wd->metric[0]], MNAME[wd->metric[1]]);
    for (char *p = lbl; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    int y = py + (int)(ls * 0.95f);
    cv_text(&cv, F_BOLD, ls, px, y, lbl, MCOL[wd->metric[0]], 255, ls * 0.14f, w / 2);
    char right[80] = "";
    if (single && wd->metric[0] == M_CPU) snprintf(right, sizeof right, "%.40s", S.cpuname);
    else if (single && wd->metric[0] == M_GPU) snprintf(right, sizeof right, "%.40s", S.gpuname);
    else if (net && S.ping >= 0) snprintf(right, sizeof right, "Ping %.0f ms", S.ping);
    if (right[0]) {
        int rw = cv_text_w(F_REGULAR, ls, right, 0);
        if (rw > w / 2 - px) rw = w / 2 - px;
        cv_text(&cv, F_REGULAR, ls, w - px - rw, y, right, MUTED, 255, 0, w / 2 - px);
    }
    y += (int)(bs * 1.05f);
    char v[24], u[12], sub[64];
    if (single) {
        metric_text(wd->metric[0], v, sizeof v, u, sizeof u, sub, sizeof sub);
        int x = cv_text(&cv, F_MEDIUM, bs, px, y, v, TEXT, 255, -bs * 0.02f, 0);
        x = cv_text(&cv, F_REGULAR, ss, x + P(3), y, u, MUTED, 255, 0, 0);
        if (sub[0]) {
            int cw = cv_text_w(F_REGULAR, P(11), sub, 0) + P(12);
            cv_rrect(&cv, x + P(8), y - P(14), cw, P(18), P(9), 0xffffff, 18);
            cv_text(&cv, F_REGULAR, P(11), x + P(14), y - P(1), sub, MUTED, 255, 0, 0);
        }
    } else {
        int x = px;
        for (int i = 0; i < wd->nmetric; i++) {
            int m = wd->metric[i];
            metric_text(m, v, sizeof v, u, sizeof u, sub, sizeof sub);
            char t[48];
            snprintf(t, sizeof t, "%s %s %s", m == M_DOWN ? "\xe2\x86\x93" : m == M_UP ? "\xe2\x86\x91" : MLABEL[m], v, u);
            x = cv_text(&cv, F_MEDIUM, P(clampf(13, 0.10f * hc, 20)), x, y, t, MCOL[m], 255, 0, 0) + P(12);
        }
    }
    int top = y + P(8), ch = h - top - P(6);
    for (int g = 1; g < 4; g++) cv_rect(&cv, px, top + ch * g / 4, w - 2 * px, 1 + (int)(SC / 3), 0xffffff, 10);
    for (int i = wd->nmetric - 1; i >= 0; i--) spark(wd->metric[i], P(2), top, w - P(4), ch, 230, 50);
}

static void draw_meters(int w, int h) {
    card(w, h);
    int px = P(12), py = P(10), ls = P(11), rs = P(12);
    int y = py + (int)(ls * 0.95f);
    cv_text(&cv, F_BOLD, ls, px, y, "MEMORY", MCOL[M_RAM], 255, ls * 0.14f, 0);
    char up[48];
    long s = (long)S.uptime, d = s / 86400, hr = s % 86400 / 3600, mi = s % 3600 / 60;
    if (d) snprintf(up, sizeof up, "Up %ld d %ld h", d, hr); else if (hr) snprintf(up, sizeof up, "Up %ld h %ld min", hr, mi); else snprintf(up, sizeof up, "Up %ld min", mi);
    cv_text(&cv, F_REGULAR, ls, w - px - cv_text_w(F_REGULAR, ls, up, 0), y, up, MUTED, 255, 0, 0);
    struct { char name[40], txt[32]; double pct; uint32_t c; } r[6];
    int n = 0;
    snprintf(r[n].name, sizeof r[n].name, "RAM"); snprintf(r[n].txt, sizeof r[n].txt, "%.1f / %.0f GB", S.ram, S.ramt);
    r[n].pct = S.ramt > 0 ? S.ram / S.ramt * 100 : 0; r[n++].c = MCOL[M_RAM];
    if (S.has_gpu && S.vramt > 0) {
        snprintf(r[n].name, sizeof r[n].name, "VRAM"); snprintf(r[n].txt, sizeof r[n].txt, "%.1f / %.0f GB", S.vram, S.vramt);
        r[n].pct = S.vram / S.vramt * 100; r[n++].c = MCOL[M_GPU];
    }
    for (int i = 0; i < S.ndisk && n < 6; i++) {
        snprintf(r[n].name, sizeof r[n].name, "%s", !strcmp(S.disk[i].name, "/") ? "DISK" : S.disk[i].name);
        snprintf(r[n].txt, sizeof r[n].txt, "%.0f%%", S.disk[i].pct);
        r[n].pct = S.disk[i].pct; r[n++].c = 0x94a3b8;
    }
    int top = y + P(10), avail = h - top - P(8), rowh = n ? avail / n : avail;
    for (int i = 0; i < n; i++) {
        int ry = top + i * rowh + rowh / 2;
        cv_text(&cv, F_REGULAR, rs, px, ry - P(2), r[i].name, TEXT, 255, 0, w / 2);
        cv_text(&cv, F_REGULAR, rs, w - px - cv_text_w(F_REGULAR, rs, r[i].txt, 0), ry - P(2), r[i].txt, MUTED, 255, 0, 0);
        int tw = w - 2 * px, th = P(6);
        cv_rrect(&cv, px, ry + P(4), tw, th, th / 2, 0xffffff, 20);
        int fw = (int)(tw * clampf(0, (float)r[i].pct / 100, 1));
        if (fw > th) cv_rrect(&cv, px, ry + P(4), fw, th, th / 2, r[i].c, 255);
    }
}

/* simple vector weather icons (the phonebox app uses emoji; no colour emoji font here yet) */
static void wx_icon(float cx, float cy, float s, int code, int day) {
    int cloud = code >= 2, rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
    int snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    if (code <= 2) {
        float ox = cloud ? -s * 0.18f : 0, oy = cloud ? -s * 0.12f : 0;
        cv_circle(&cv, cx + ox, cy + oy, s * (day ? 0.30f : 0.26f), day ? 0xfbbf24 : 0xe2e8f0, 255);
    }
    if (cloud) {
        uint32_t c = code >= 45 ? 0xcbd5e1 : 0xe2e8f0;
        cv_circle(&cv, cx - s * 0.20f, cy + s * 0.06f, s * 0.20f, c, 255);
        cv_circle(&cv, cx + s * 0.05f, cy - s * 0.06f, s * 0.26f, c, 255);
        cv_circle(&cv, cx + s * 0.28f, cy + s * 0.08f, s * 0.18f, c, 255);
        cv_rrect(&cv, (int)(cx - s * 0.40f), (int)(cy + s * 0.02f), (int)(s * 0.86f), (int)(s * 0.24f), (int)(s * 0.12f), c, 255);
    }
    for (int i = 0; i < 3 && (rain || snow); i++) {
        float x = cx - s * 0.22f + i * s * 0.22f, y = cy + s * 0.36f;
        if (rain) cv_line(&cv, x, y, x - s * 0.06f, y + s * 0.16f, fmaxf(2, s * 0.05f), 0x60a5fa, 255);
        else cv_circle(&cv, x, y + s * 0.08f, s * 0.05f, 0xffffff, 255);
    }
}

static const char *wx_text(int c) {
    if (c == 0) return "Clear";
    if (c == 1) return "Mainly clear";
    if (c == 2) return "Partly cloudy";
    if (c == 3) return "Overcast";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c == 85 || c == 86) return "Snow showers";
    if (c >= 95) return "Thunderstorm";
    return "\xe2\x80\x94";
}

/* Timezone: a manual override in the settings store ("tz_off_min", e.g. 60 for CET), else the configured
 * weather place's offset (open-meteo, timezone=auto): the phonebox app uses the phone's own zone, which PhoneKit
 * doesn't have. */
static long tz_seconds(void) {
    const char *o = store_get("tz_off_min", "");
    return o[0] ? atol(o) * 60 : tz_off;
}

static time_t local_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + pc_ts_skew;
    return (time_t)(ms / 1000) + tz_seconds();
}

static void w_clock(const struct pbw *wd, int w, int h) {
    float hc = h / SC, wc = w / SC;
    int y = 0;
    if (wd->status) { /* status row: connection dot, host, route, the phone's own battery */
        int ss = P(clampf(10, 0.05f * hc, 12.5f));
        y = P(4) + ss;
        cv_circle(&cv, P(3.5f), y - ss * 0.35f, P(3.5f), connected ? 0x22c55e : 0xef4444, 255);
        char st[96], bat[16] = "";
        FILE *f = fopen("/sys/class/power_supply/battery/capacity", "r");
        if (f) { int b; if (fscanf(f, "%d", &b) == 1) snprintf(bat, sizeof bat, " \xc2\xb7 %d%%", b); fclose(f); }
        snprintf(st, sizeof st, "%s \xc2\xb7 USB%s", S.host[0] ? S.host : "PC", bat);
        for (char *p = st; *p && *p != ' '; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        cv_text(&cv, F_REGULAR, ss, P(12), y, st, MUTED, 255, ss * 0.02f, w - P(12));
        y += P(6);
    }
    time_t t = local_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    float tf = wd->weather ? 0.48f : 0.6f;
    int ts = (int)(fminf(tf * hc, 0.30f * wc) * SC);
    char hh[4], mm[4], sec[4];
    snprintf(hh, sizeof hh, "%02d", tm.tm_hour); snprintf(mm, sizeof mm, "%02d", tm.tm_min); snprintf(sec, sizeof sec, "%02d", tm.tm_sec);
    int base = y + (int)(ts * 0.80f);
    float ls = -ts * 0.04f;
    int x = cv_text(&cv, F_LIGHT, ts, -(int)(ts * 0.03f), base, hh, TEXT, 255, ls, 0);
    x = cv_text(&cv, F_LIGHT, ts, x, base, ":", TEXT, 140, ls, 0);
    x = cv_text(&cv, F_LIGHT, ts, x, base, mm, TEXT, 255, ls, 0);
    int secs = ts / 5;
    cv_text(&cv, F_REGULAR, secs, x + (int)(ts * 0.09f), y + (int)(ts * 0.10f) + secs, sec, MUTED, 255, 0, 0);
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    char date[48];
    snprintf(date, sizeof date, "%s %d %s", DAY[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    int ds = P(clampf(11, 0.075f * hc, 19));
    y = base + (int)(ds * 1.45f);
    cv_text(&cv, F_REGULAR, ds, P(2), y, date, 0xc9d1db, 255, 0, w);
    if (wd->weather && W.ok) {
        int is = P(clampf(18, 0.13f * hc, 34));
        y += P(clampf(2, 0.04f * hc, 14)) + is;
        wx_icon(P(2) + is * 0.6f, y - is * 0.38f, is * 1.2f, W.code, W.is_day);
        char tt[16];
        snprintf(tt, sizeof tt, "%.0f\xc2\xb0", W.t);
        int tx = cv_text(&cv, F_LIGHT, is, P(2) + (int)(is * 1.5f), y, tt, TEXT, 255, -is * 0.02f, 0);
        int ms = P(clampf(10, 0.054f * hc, 13));
        char l1[64], l2[64];
        snprintf(l1, sizeof l1, "%s%s%s", wx_text(W.code), W.city[0] ? " \xc2\xb7 " : "", W.city);
        snprintf(l2, sizeof l2, "\xe2\x86\x91%.0f\xc2\xb0 \xe2\x86\x93%.0f\xc2\xb0 \xc2\xb7 Feels like %.0f\xc2\xb0", W.tmax, W.tmin, W.feels);
        cv_text(&cv, F_REGULAR, ms, tx + P(8), y - (int)(ms * 1.25f), l1, TEXT, 255, 0, w - tx - P(8));
        cv_text(&cv, F_REGULAR, ms, tx + P(8), y + P(1), l2, MUTED, 255, 0, w - tx - P(8));
    }
}

/* hit boxes (screen coordinates) for the touch handler */
struct hit { int x, y, w, h; char action[128]; };
#define MAXHIT 48
static struct hit hits[MAXHIT];
static int nhits;
static char pressed_action[128];

static void add_hit(int ox, int oy, int x, int y, int w, int h, const char *a) {
    if (nhits >= MAXHIT) return;
    hits[nhits] = (struct hit){ox + x, oy + y, w, h, ""};
    snprintf(hits[nhits].action, sizeof hits[nhits].action, "%s", a);
    nhits++;
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
    int minx = (int)fminf(x0, fminf(x1, x2)), maxx = (int)fmaxf(x0, fmaxf(x1, x2)) + 1;
    int miny = (int)fminf(y0, fminf(y1, y2)), maxy = (int)fmaxf(y0, fmaxf(y1, y2)) + 1;
    for (int j = miny; j <= maxy; j++)
        for (int i = minx; i <= maxx; i++) {
            int cov = 0;
            for (int s = 0; s < 4; s++) {
                float px = i + 0.25f + 0.5f * (s & 1), py = j + 0.25f + 0.5f * (s >> 1);
                float d0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
                float d1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
                float d2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
                if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) cov++;
            }
            if (cov) cv_rect(&cv, i, j, 1, 1, c, cov == 4 ? 255 : cov * 64);
        }
}

static void draw_media(int ox, int oy, int w, int h) {
    card(w, h);
    float hc = h / SC, wc = w / SC;
    int pad = P(clampf(10, 0.07f * hc, 18));
    int ctl = P(hc >= 230 && wc >= 420 ? 62 : 50);
    int artsz = (int)fminf(h - 2 * pad - ctl - P(44), w * 0.32f);
    if (artsz < P(40)) artsz = 0;
    int tx = pad;
    if (artsz) {
        if (art_raw_id == S.art && S.art > 0 && art_raw.len && (art_id_loaded != S.art || art.w != artsz))
            art_id_loaded = cv_image(&art, (unsigned char *)art_raw.data, (int)art_raw.len, artsz, artsz) == 0 ? S.art : -1;
        if (art_id_loaded > 0 && art_id_loaded == S.art && art.w == artsz) cv_blit_rounded(&cv, &art, pad, pad, P(12));
        else {
            cv_rrect(&cv, pad, pad, artsz, artsz, P(12), 0xffffff, 16);
            cv_circle(&cv, pad + artsz / 2.0f, pad + artsz / 2.0f, artsz * 0.12f, 0xffffff, 40);
        }
        tx = pad + artsz + P(18);
    }
    int as = P(clampf(9, 0.045f * hc, 13)), tsz = P(clampf(13, 0.09f * hc, 30)), rs = P(clampf(11, 0.06f * hc, 19));
    int y = pad + (artsz ? artsz / 2 - (as + tsz + rs) / 2 : 0) + as;
    char app[40];
    snprintf(app, sizeof app, "%s", S.media && S.app[0] ? S.app : "Media");
    for (char *p = app; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    cv_text(&cv, F_MEDIUM, as, tx, y, app, 0x2dd4bf, 255, as * 0.12f, w - tx - pad);
    y += (int)(tsz * 1.25f);
    cv_text(&cv, F_MEDIUM, tsz, tx, y, S.media ? (S.title[0] ? S.title : "Unknown track") : "Nothing playing", TEXT, 255, 0, w - tx - pad);
    y += (int)(rs * 1.3f);
    cv_text(&cv, F_REGULAR, rs, tx, y, S.media ? (S.artist[0] ? S.artist : "\xe2\x80\x94") : "Press play to resume", MUTED, 255, 0, w - tx - pad);
    /* progress */
    int pyb = h - pad - ctl - P(30);
    int bw = w - 2 * pad, bh = P(4);
    long pos = S.pos_ms + (S.playing ? now_ms() - S.at_ms : 0);
    float f = S.dur_ms > 0 ? clampf(0, (float)pos / S.dur_ms, 1) : 0;
    cv_rrect(&cv, pad, pyb, bw, bh, bh / 2, 0xffffff, 31);
    if (f > 0) { cv_rrect(&cv, pad, pyb, (int)(bw * f), bh, bh / 2, TEXT, 255); cv_circle(&cv, pad + bw * f, pyb + bh / 2.0f, P(6), TEXT, 255); }
    char a[16], b[16];
    snprintf(a, sizeof a, "%ld:%02ld", pos / 60000, pos / 1000 % 60);
    snprintf(b, sizeof b, "%ld:%02ld", S.dur_ms / 60000, S.dur_ms / 1000 % 60);
    int sm = P(12);
    cv_text(&cv, F_REGULAR, sm, pad, pyb + P(20), a, MUTED, 255, 0, 0);
    cv_text(&cv, F_REGULAR, sm, w - pad - cv_text_w(F_REGULAR, sm, b, 0), pyb + P(20), b, MUTED, 255, 0, 0);
    /* controls: prev, big play/pause circle, next, volume */
    int cy = h - pad - ctl / 2;
    float cx = pad + ctl * 1.6f, ic = ctl * 0.22f;
    uint32_t icol = rgb_mix(BG, TEXT, S.media ? 255 : 90);
    float px0 = cx - ctl * 1.05f;
    tri(px0 + ic * 0.5f, cy - ic, px0 + ic * 0.5f, cy + ic, px0 - ic * 0.9f, cy, icol);
    cv_rect(&cv, (int)(px0 - ic * 1.1f), (int)(cy - ic), P(3), (int)(2 * ic), icol, 255);
    add_hit(ox, oy, (int)(px0 - ctl / 2), cy - ctl / 2, ctl, ctl, "/api/media/prev");
    int down = !strcmp(pressed_action, "/api/media/playpause");
    cv_circle(&cv, cx, cy, ctl / 2.0f - (down ? P(3) : 0), TEXT, 255);
    if (S.playing) {
        cv_rect(&cv, (int)(cx - ic * 0.75f), (int)(cy - ic), (int)(ic * 0.55f), (int)(2 * ic), 0x0b0d10, 255);
        cv_rect(&cv, (int)(cx + ic * 0.2f), (int)(cy - ic), (int)(ic * 0.55f), (int)(2 * ic), 0x0b0d10, 255);
    } else tri(cx - ic * 0.6f, cy - ic, cx - ic * 0.6f, cy + ic, cx + ic * 1.0f, cy, 0x0b0d10);
    add_hit(ox, oy, (int)(cx - ctl / 2), cy - ctl / 2, ctl, ctl, "/api/media/playpause");
    float nx = cx + ctl * 1.05f;
    tri(nx - ic * 0.5f, cy - ic, nx - ic * 0.5f, cy + ic, nx + ic * 0.9f, cy, icol);
    cv_rect(&cv, (int)(nx + ic * 0.9f), (int)(cy - ic), P(3), (int)(2 * ic), icol, 255);
    add_hit(ox, oy, (int)(nx - ctl / 2), cy - ctl / 2, ctl, ctl, "/api/media/next");
    if (S.has_vol && w > ctl * 6) {
        int vx = (int)(nx + ctl * 1.0f), bx = vx + P(18), vw = w - pad - bx - P(34);
        cv_rect(&cv, vx, cy - P(3), P(4), P(6), MUTED, 255);
        tri(vx + P(3), cy - P(3), vx + P(3), cy + P(3), vx + P(9), cy - P(8), MUTED);
        tri(vx + P(3), cy + P(3), vx + P(9), cy + P(8), vx + P(9), cy - P(8), MUTED);
        if (vw > P(40)) {
            float vf = clampf(0, (float)S.vol / 100, 1);
            cv_rrect(&cv, bx, cy - P(2), vw, P(4), P(2), 0xffffff, 31);
            cv_rrect(&cv, bx, cy - P(2), (int)(vw * vf), P(4), P(2), TEXT, 255);
            cv_circle(&cv, bx + vw * vf, cy, P(9), TEXT, 255);
            char vt[8];
            snprintf(vt, sizeof vt, "%.0f", S.vol);
            cv_text(&cv, F_REGULAR, P(13), w - pad - cv_text_w(F_REGULAR, P(13), vt, 0), cy + P(5), vt, MUTED, 255, 0, 0);
        }
    }
}

static void draw_shortcuts(const struct pbw *wd, int ox, int oy, int w, int h) {
    int cols = wd->sc_cols < 1 ? 4 : wd->sc_cols, n = nsc;
    if (!land && cols > 3) cols = 3;
    int rows = (n + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int gap = P(8);
    int tw = (w - gap * (cols - 1)) / cols, th = (h - gap * (rows - 1)) / rows;
    for (int i = 0; i < n; i++) {
        int x = (i % cols) * (tw + gap), y = (i / cols) * (th + gap);
        char act[48];
        snprintf(act, sizeof act, "/api/run/%.30s", sc[i].id);
        int down = !strcmp(pressed_action, act);
        cv_rrect(&cv, x, y, tw, th, P(14), sc[i].color, down ? 90 : 38);
        cv_rrect_stroke(&cv, x, y, tw, th, P(14), fmaxf(1.5f, SC * 0.6f), sc[i].color, 70);
        float r = fminf(tw, th) * 0.2f;
        char ini[8];
        int k = 0;
        const char *l = sc[i].label;
        while (*l == ' ') l++;
        if (*l) { ini[k++] = (char)(*l >= 'a' && *l <= 'z' ? *l - 32 : *l); while ((l[1] & 0xc0) == 0x80 && k < 6) ini[k++] = *++l; }
        ini[k] = 0;
        cv_circle(&cv, x + tw / 2.0f, y + th * 0.42f, r, sc[i].color, 255);
        int is = (int)(r * 1.1f);
        cv_text(&cv, F_BOLD, is, x + tw / 2 - cv_text_w(F_BOLD, is, ini, 0) / 2, (int)(y + th * 0.42f + is * 0.36f), ini, 0xffffff, 255, 0, 0);
        int ls = P(clampf(10, th / SC * 0.13f, 14));
        int lw = cv_text_w(F_MEDIUM, ls, sc[i].label, 0);
        if (lw > tw - P(8)) lw = tw - P(8);
        cv_text(&cv, F_MEDIUM, ls, x + (tw - lw) / 2, y + th - P(10), sc[i].label, TEXT, 255, 0, tw - P(8));
        add_hit(ox, oy, x, y, tw, th, act);
    }
}

static void draw_note(const struct pbw *wd, int w, int h) {
    card(w, h);
    int pad = P(14), fs = P(15), y = pad + fs;
    const char *t = wd->text[0] ? wd->text : wd->tname;
    char line[160];
    while (*t && y < h - pad / 2) {
        int n = 0;
        while (t[n] && t[n] != '\n') n++;
        snprintf(line, sizeof line, "%.*s", n, t);
        cv_text(&cv, F_REGULAR, fs, pad, y, line, TEXT, 255, 0, w - 2 * pad);
        y += (int)(fs * 1.45f);
        t += n + (t[n] == '\n');
    }
}

static void draw_forecast(int w, int h) {
    card(w, h);
    int pad = P(12);
    if (!W.ok) { cv_text(&cv, F_REGULAR, P(13), pad, pad + P(13), "Loading weather\xe2\x80\xa6", MUTED, 255, 0, 0); return; }
    static const char *const D[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int n = W.nd < 1 ? 1 : W.nd;
    time_t t = local_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    int vertical = h > w;
    if (vertical && n > 5) n = 5;
    for (int i = 0; i < n; i++) {
        const char *dn = i == 0 ? "Today" : D[(tm.tm_wday + i) % 7];
        char hi[12], lo[12];
        snprintf(hi, sizeof hi, "%.0f\xc2\xb0", W.dmax[i]); snprintf(lo, sizeof lo, "%.0f\xc2\xb0", W.dmin[i]);
        if (vertical) {
            int rh = (h - 2 * pad) / n, y = pad + i * rh + rh / 2;
            cv_text(&cv, F_REGULAR, P(13), pad, y + P(5), dn, TEXT, 255, 0, 0);
            wx_icon(w * 0.5f, y, rh * 0.7f, W.dcode[i], 1);
            cv_text(&cv, F_MEDIUM, P(13), w - pad - P(70), y + P(5), hi, TEXT, 255, 0, 0);
            cv_text(&cv, F_REGULAR, P(13), w - pad - P(32), y + P(5), lo, MUTED, 255, 0, 0);
        } else {
            int cw = (w - 2 * pad) / n, x = pad + i * cw + cw / 2;
            int fs = P(clampf(10, h / SC * 0.08f, 14));
            cv_text(&cv, F_REGULAR, fs, x - cv_text_w(F_REGULAR, fs, dn, 0) / 2, pad + fs, dn, MUTED, 255, 0, 0);
            wx_icon(x, h * 0.48f, fminf(cw * 0.7f, h * 0.35f), W.dcode[i], 1);
            cv_text(&cv, F_MEDIUM, fs, x - cv_text_w(F_MEDIUM, fs, hi, 0) / 2, (int)(h - pad - fs * 1.4f), hi, TEXT, 255, 0, 0);
            cv_text(&cv, F_REGULAR, fs, x - cv_text_w(F_REGULAR, fs, lo, 0) / 2, h - pad, lo, MUTED, 255, 0, 0);
        }
    }
}

/* A card with a coloured caps title, a right-hand note and rows: text, sub text / status, optional bar. */
static void draw_list(int w, int h, const char *title, uint32_t tcol, const char *note, const struct row *r, int n,
                      int bars, const char *empty) {
    card(w, h);
    int px = P(12), py = P(10), ls = P(11), fs = P(clampf(11, h / SC * 0.055f, 14)), ss = P(11);
    int y = py + (int)(ls * 0.95f);
    cv_text(&cv, F_BOLD, ls, px, y, title, tcol, 255, ls * 0.14f, w / 2);
    if (note && *note) cv_text(&cv, F_REGULAR, ls, w - px - cv_text_w(F_REGULAR, ls, note, 0), y, note, MUTED, 255, 0, w / 2 - px);
    y += P(10);
    if (!n) {
        const char *e = empty;
        char line[64];
        int ly = y + fs + P(4);
        while (*e && ly < h - P(8)) { /* wrap on spaces */
            int k = 0, last = -1;
            while (e[k] && cv_text_w(F_REGULAR, fs, (snprintf(line, sizeof line, "%.*s", k + 1, e), line), 0) < w - 2 * px && k < 62) { if (e[k] == ' ') last = k; k++; }
            if (e[k] && last > 0) k = last + 1;
            snprintf(line, sizeof line, "%.*s", k, e);
            cv_text(&cv, F_REGULAR, fs, px, ly, line, MUTED, 255, 0, w - 2 * px);
            ly += (int)(fs * 1.4f);
            e += k ? k : 1;
        }
        return;
    }
    int rowh = (int)(fs * (bars ? 3.5f : 2.5f));
    for (int i = 0; i < n && y + rowh <= h - P(4); i++, y += rowh) {
        int tb = y + fs + P(2);
        int cw = r[i].c[0] ? cv_text_w(F_MEDIUM, ss, r[i].c, 0) + P(14) : 0;
        if (!bars && cw) { /* status chip */
            cv_rrect(&cv, w - px - cw, tb - ss - P(3), cw, ss + P(9), (ss + P(9)) / 2, r[i].col, 40);
            cv_text(&cv, F_MEDIUM, ss, w - px - cw + P(7), tb, r[i].c, r[i].col, 255, 0, 0);
        }
        int bw = bars ? cv_text_w(F_MEDIUM, fs, r[i].b, 0) : 0;
        cv_text(&cv, F_REGULAR, fs, px, tb, r[i].a, TEXT, 255, 0, w - 2 * px - (bars ? bw + P(10) : cw + P(8)));
        if (bars) {
            cv_text(&cv, F_MEDIUM, fs, w - px - bw, tb, r[i].b, TEXT, 255, 0, 0);
            int ty = tb + P(6), tw = w - 2 * px, th = P(5);
            if (r[i].pct < 0) { /* provider without numbers: its message instead of a bar */
                cv_text(&cv, F_REGULAR, ss, px, ty + ss, r[i].err, MUTED, 255, 0, w - 2 * px);
                continue;
            }
            cv_rrect(&cv, px, ty, tw, th, th / 2, 0xffffff, 20);
            int fw = (int)(tw * clampf(0, (float)r[i].pct / 100, 1));
            if (fw > th) cv_rrect(&cv, px, ty, fw, th, th / 2, r[i].col, 255);
            if (r[i].c[0]) cv_text(&cv, F_REGULAR, ss, px, ty + th + ss + P(2), r[i].c, MUTED, 255, 0, w - 2 * px);
        } else if (r[i].b[0]) cv_text(&cv, F_REGULAR, ss, px, tb + ss + P(4), r[i].b, MUTED, 255, 0, w - 2 * px);
    }
}

static void draw_procs(int w, int h) {
    card(w, h);
    int px = P(12), py = P(10), ls = P(11), fs = P(clampf(11, h / SC * 0.05f, 14));
    int y = py + (int)(ls * 0.95f);
    cv_text(&cv, F_BOLD, ls, px, y, "PROCESSES", MCOL[M_CPU], 255, ls * 0.14f, 0);
    char note[32];
    snprintf(note, sizeof note, "%d running", p_total);
    cv_text(&cv, F_REGULAR, ls, w - px - cv_text_w(F_REGULAR, ls, note, 0), y, note, MUTED, 255, 0, 0);
    int c1 = w - px - P(150), c2 = w - px - P(60);
    y += P(14) + ls;
    cv_text(&cv, F_MEDIUM, ls, px, y, "NAME", DIM, 255, ls * 0.1f, 0);
    cv_text(&cv, F_MEDIUM, ls, c1, y, "CPU", DIM, 255, ls * 0.1f, 0);
    cv_text(&cv, F_MEDIUM, ls, c2, y, "MEMORY", DIM, 255, ls * 0.1f, 0);
    int rowh = (int)(fs * 2.0f);
    for (int i = 0; i < np && y + rowh <= h - P(6); i++) {
        y += rowh;
        cv_rect(&cv, px, y - fs - P(8), w - 2 * px, 1 + (int)(SC / 3), 0xffffff, 12);
        cv_text(&cv, F_REGULAR, fs, px, y, p_rows[i].a, TEXT, 255, 0, c1 - px - P(10));
        int bw = (int)(P(40) * clampf(0, (float)p_rows[i].pct / 100, 1));
        cv_rrect(&cv, c1, y - fs / 2 - P(2), P(40), P(4), P(2), 0xffffff, 20);
        if (bw > P(4)) cv_rrect(&cv, c1, y - fs / 2 - P(2), bw, P(4), P(2), MCOL[M_CPU], 255);
        cv_text(&cv, F_MEDIUM, fs, c1 + P(46), y, p_rows[i].b, TEXT, 255, 0, 0);
        cv_text(&cv, F_REGULAR, fs, c2, y, p_rows[i].c, MUTED, 255, 0, 0);
    }
    if (!np) cv_text(&cv, F_REGULAR, fs, px, y + rowh, "Loading\xe2\x80\xa6", MUTED, 255, 0, 0);
}

static void draw_other(const struct pbw *wd, int w, int h) {
    card(w, h);
    char t[32];
    snprintf(t, sizeof t, "%s", wd->tname);
    if (t[0] >= 'a' && t[0] <= 'z') t[0] -= 32;
    int fs = P(14);
    cv_text(&cv, F_MEDIUM, fs, (w - cv_text_w(F_MEDIUM, fs, t, 0)) / 2, h / 2, t, MUTED, 255, 0, w);
    const char *s = "coming to PhoneKit";
    cv_text(&cv, F_REGULAR, P(11), (w - cv_text_w(F_REGULAR, P(11), s, 0)) / 2, h / 2 + P(18), s, DIM, 255, 0, w);
}

static int visible(void) { return pb_owns() && hm_owner() == OWN_PAGE && ui_page() == UI_PAGE_PANEL; }

static void draw_widget(int i) {
    struct pbw *wd = &pg[cur].w[i];
    int x, y, w, h;
    grid_rect(wd, &x, &y, &w, &h);
    if (cv_alloc(&cv, w, h) < 0) return;
    cv_clear(&cv, BG);
    int keep = 0; /* this widget's hit boxes are re-added as it draws */
    for (int k = 0; k < nhits; k++)
        if (!(hits[k].x >= x && hits[k].y >= y && hits[k].x < x + w && hits[k].y < y + h)) hits[keep++] = hits[k];
    nhits = keep;
    switch (wd->type) {
    case T_CLOCK: w_clock(wd, w, h); break;
    case T_STAT: draw_stat(wd, w, h); break;
    case T_GAUGE: draw_gauge(wd, w, h); break;
    case T_CHART: draw_chart(wd, w, h); break;
    case T_METERS: draw_meters(w, h); break;
    case T_MEDIA: draw_media(x, y, w, h); break;
    case T_SHORTCUTS: draw_shortcuts(wd, x, y, w, h); break;
    case T_NOTE: draw_note(wd, w, h); break;
    case T_FORECAST: draw_forecast(w, h); break;
    case T_QUOTAS: draw_list(w, h, q_head[0] ? q_head : "AI QUOTAS", 0xa78bfa, NULL, q_rows, nq, 1, "No quota data yet"); break;
    case T_SESSIONS: {
        char note[24];
        snprintf(note, sizeof note, "%d active", ns);
        draw_list(w, h, "SESSIONS", 0x2dd4bf, note, s_rows, ns, 0, "No agent sessions running");
        break;
    }
    case T_GITHUB: {
        char note[24] = "";
        if (g_notif) snprintf(note, sizeof note, "%d notifications", g_notif);
        draw_list(w, h, "GITHUB", 0xeef2f6, note, g_rows, ng, 0, g_err[0] ? g_err : "Nothing waiting for you");
        break;
    }
    case T_PROCS: draw_procs(w, h); break;
    default: draw_other(wd, w, h);
    }
    fb_blit(fb, &cv, x, y);
}

static void draw_dots(void) {
    int dh = P(17);
    if (cv_alloc(&cv, fb->w, dh) < 0) return;
    cv_clear(&cv, BG);
    int dw = P(6), on = P(16), gap = P(6);
    int total = npg * dw + (on - dw) + (npg - 1) * gap, x = (fb->w - total) / 2;
    for (int i = 0; i < npg; i++) {
        int ww = i == cur ? on : dw;
        cv_rrect(&cv, x, dh / 2 - dw / 2, ww, dw, dw / 2, 0xffffff, i == cur ? 178 : 56);
        x += ww + gap;
    }
    fb_blit(fb, &cv, 0, fb->h - dh);
}

static void geometry(void) {
    land = fb->w > fb->h;
    SC = (land ? fb->h : fb->w) / 360.0f;
}

void pb_draw(void) {
    if (!fb || !fb->mem || !have_cfg) return;
    if (gone && !strcmp(standby_mode(), "ambient")) { draw_ambient(); return; }
    geometry();
    fb_fill(fb, 0, 0, fb->w, fb->h, fb_color(fb, BG >> 16, BG >> 8 & 255, BG & 255));
    nhits = 0;
    for (int i = 0; i < pg[cur].n; i++) draw_widget(i);
    draw_dots();
    draw_overlays();
    fb_present(fb);
}

static void redraw_types(int mask) {
    if (!visible()) return;
    if (modal()) return; /* the approval card stays on top, untouched */
    if (gone && !strcmp(standby_mode(), "ambient")) { if (mask & (1 << T_CLOCK)) draw_ambient(); return; }
    geometry();
    for (int i = 0; i < pg[cur].n; i++) if (mask & (1 << pg[cur].w[i].type)) draw_widget(i);
    if (toast.until > now_ms()) draw_toast();
    fb_present(fb);
}

/* Full-screen ambient clock: time, date, the phone's battery; drawn dim (the backlight is dimmed too). */
static void draw_ambient(void) {
    geometry();
    if (cv_alloc(&cv, fb->w, fb->h) < 0) return;
    cv_clear(&cv, 0x000000);
    time_t t = local_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    char hm[8], date[48], bat[48] = "";
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    snprintf(date, sizeof date, "%s %d %s", DAY[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    FILE *f = fopen("/sys/class/power_supply/battery/capacity", "r");
    if (f) { int b; if (fscanf(f, "%d", &b) == 1) snprintf(bat, sizeof bat, "PC offline \xc2\xb7 phone %d%%", b); fclose(f); }
    int ts = (int)fminf(fb->h * 0.42f, fb->w * 0.30f);
    int tw = cv_text_w(F_LIGHT, ts, hm, -ts * 0.04f);
    int base = fb->h / 2 + ts / 3;
    cv_text(&cv, F_LIGHT, ts, (fb->w - tw) / 2, base, hm, 0xc9d1db, 255, -ts * 0.04f, 0);
    int ds = P(18);
    cv_text(&cv, F_REGULAR, ds, (fb->w - cv_text_w(F_REGULAR, ds, date, 0)) / 2, base + (int)(ds * 2.0f), date, MUTED, 255, 0, 0);
    int bs = P(12);
    cv_text(&cv, F_REGULAR, bs, (fb->w - cv_text_w(F_REGULAR, bs, bat, 0)) / 2, fb->h - P(20), bat, DIM, 255, 0, 0);
    fb_blit(fb, &cv, 0, 0);
    fb_present(fb);
}

static const char *standby_mode(void) { return store_get("standby", "hold"); }

/* Called once a second: enter standby when the PC is gone (stream down 15 s, or USB suspended/unplugged),
 * leave it when the agent is back. */
static void standby_tick(void) {
    const char *us = gadget_state();
    /* USB suspended/unplugged means "PC gone" only if we aren't talking to it over Wi-Fi */
    int usb_away = (!strcmp(us, "suspended") || !strcmp(us, "not attached")) && !(connected && route_wifi);
    if (connected) last_seen_ms = now_ms();
    int away = usb_away || (!connected && now_ms() - last_seen_ms > 15000);
    if (away && !gone) {
        gone = 1;
        const char *m = standby_mode();
        logmsg("phonebox: PC gone (%s) -> standby %s", usb_away ? us : "no stream", m);
        if (pb_agent_muted) ;
        else if (!strcmp(m, "sleep")) power_off("PC gone");
        else if (!strcmp(m, "ambient")) { power_dim("PC gone"); if (visible()) draw_ambient(); }
        else if (visible()) pb_draw(); /* hold: the status dot turns red */
    } else if (!away && gone) {
        gone = 0;
        logmsg("phonebox: PC back");
        if (!pb_agent_muted) power_wake("PC back");
        if (visible()) pb_draw();
    }
}

/* ---------------------------------------------------------------- notifications, page switches, approvals */

struct appr { char id[48], tool[40], detail[240], project[64], source[24]; long expires; };
static struct appr ap[4];
static int nap;

static int modal(void) { return nap > 0; }

static void load_approvals(const char *s) {
    int n = json_parse(s, (int)strlen(s), tk, MAXTOK);
    int it = n > 0 ? json_get(s, tk, n, 0, "items") : -1;
    nap = 0;
    if (it < 0 || tk[it].type != J_ARR) return;
    for (int i = 0, q = it + 1; i < tk[it].size && nap < 4; i++, q = json_skip(tk, n, q)) {
        struct appr *a = &ap[nap++];
        str(s, n, q, "id", a->id, sizeof a->id); str(s, n, q, "tool", a->tool, sizeof a->tool);
        str(s, n, q, "detail", a->detail, sizeof a->detail); str(s, n, q, "project", a->project, sizeof a->project);
        str(s, n, q, "source", a->source, sizeof a->source);
        a->expires = (long)(num(s, n, q, "expiresAt", 0) / 1000);
    }
    if (nap) { if (!pb_agent_muted) power_wake("approval"); logmsg("phonebox: %d approval(s) waiting", nap); }
}

static void approval_done(const char *path) {
    const char *id = path + strlen("/api/approvals/");
    for (int i = 0; i < nap; i++)
        if (!strncmp(ap[i].id, id, strlen(ap[i].id)) && id[strlen(ap[i].id)] == '?') {
            memmove(&ap[i], &ap[i + 1], sizeof ap[0] * (size_t)(nap - i - 1));
            nap--;
            break;
        }
}

static void draw_toast(void) {
    if (toast.until <= now_ms()) return;
    int w = (int)fminf(fb->w * 0.62f, (float)P(420)), h = P(toast.title[0] && toast.text[0] ? 54 : 38);
    if (cv_alloc(&cv, w, h) < 0) return;
    cv_clear(&cv, BG);
    cv_rrect(&cv, 0, 0, w, h, P(12), 0x1c2129, 255);
    cv_rrect_stroke(&cv, 0, 0, w, h, P(12), fmaxf(1.5f, SC * 0.6f), toast.col, 120);
    cv_rrect(&cv, P(10), P(10), P(4), h - P(20), P(2), toast.col, 255);
    int y = toast.title[0] && toast.text[0] ? P(22) : h / 2 + P(5);
    if (toast.title[0]) { cv_text(&cv, F_MEDIUM, P(14), P(24), y, toast.title, TEXT, 255, 0, w - P(34)); y += P(19); }
    if (toast.text[0]) cv_text(&cv, F_REGULAR, P(13), P(24), y, toast.text, toast.title[0] ? MUTED : TEXT, 255, 0, w - P(34));
    fb_blit(fb, &cv, (fb->w - w) / 2, fb->h - h - P(24));
}

static void draw_approval(void) {
    struct appr *a = &ap[0];
    int w = (int)fminf(fb->w * 0.8f, (float)P(560)), h = (int)fminf(fb->h * 0.82f, (float)P(290));
    int ox = (fb->w - w) / 2, oy = (fb->h - h) / 2;
    if (cv_alloc(&cv, w, h) < 0) return;
    cv_clear(&cv, BG);
    cv_rrect(&cv, 0, 0, w, h, P(18), 0x151a21, 255);
    cv_rrect_stroke(&cv, 0, 0, w, h, P(18), fmaxf(1.5f, SC * 0.6f), 0xf59e0b, 140);
    int px = P(18), y = P(30);
    char head[64];
    snprintf(head, sizeof head, "%s WANTS TO USE", a->source[0] ? a->source : "CLAUDE CODE");
    for (char *c = head; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    cv_text(&cv, F_BOLD, P(11), px, y, head, 0xf59e0b, 255, P(11) * 0.14f, w - 2 * px);
    if (nap > 1) { char m[16]; snprintf(m, sizeof m, "1 of %d", nap); cv_text(&cv, F_REGULAR, P(11), w - px - cv_text_w(F_REGULAR, P(11), m, 0), y, m, MUTED, 255, 0, 0); }
    y += P(30);
    cv_text(&cv, F_MEDIUM, P(24), px, y, a->tool[0] ? a->tool : "a tool", TEXT, 255, 0, w - 2 * px);
    y += P(10);
    /* detail: wrapped monospace-ish lines */
    int fs = P(13), lines = 0, maxl = (h - y - P(84)) / (int)(fs * 1.4f);
    const char *d = a->detail;
    char line[120];
    while (*d && lines < maxl) {
        int k = 0, last = -1;
        while (d[k] && d[k] != '\n' && k < 110) {
            snprintf(line, sizeof line, "%.*s", k + 1, d);
            if (cv_text_w(F_MONO, fs, line, 0) > w - 2 * px - P(16)) break;
            if (d[k] == ' ') last = k;
            k++;
        }
        if (d[k] && d[k] != '\n' && last > 0) k = last + 1;
        snprintf(line, sizeof line, "%.*s", k, d);
        y += (int)(fs * 1.4f);
        cv_text(&cv, F_MONO, fs, px + P(8), y, line, 0xc9d1db, 255, 0, w - 2 * px - P(8));
        d += k ? k : 1;
        if (*d == '\n') d++;
        lines++;
    }
    if (a->project[0]) cv_text(&cv, F_REGULAR, P(12), px, h - P(74), a->project, MUTED, 255, 0, w - 2 * px);
    int bw = (w - 3 * px) / 2, bh = P(44), by = h - bh - P(16);
    cv_rrect(&cv, px, by, bw, bh, P(12), 0xef4444, 50);
    cv_rrect_stroke(&cv, px, by, bw, bh, P(12), fmaxf(1.5f, SC * 0.6f), 0xef4444, 200);
    cv_text(&cv, F_MEDIUM, P(16), px + (bw - cv_text_w(F_MEDIUM, P(16), "Deny", 0)) / 2, by + bh / 2 + P(6), "Deny", 0xfca5a5, 255, 0, 0);
    cv_rrect(&cv, 2 * px + bw, by, bw, bh, P(12), 0x22c55e, 255);
    cv_text(&cv, F_MEDIUM, P(16), 2 * px + bw + (bw - cv_text_w(F_MEDIUM, P(16), "Allow", 0)) / 2, by + bh / 2 + P(6), "Allow", 0x052e16, 255, 0, 0);
    fb_blit(fb, &cv, ox, oy);
    nhits = 0; /* only the card answers taps while it is up */
    char act[96];
    snprintf(act, sizeof act, "/api/approvals/%s?decision=deny", a->id);
    add_hit(ox, oy, px, by, bw, bh, act);
    snprintf(act, sizeof act, "/api/approvals/%s?decision=allow", a->id);
    add_hit(ox, oy, 2 * px + bw, by, bw, bh, act);
}

static void draw_overlays(void) {
    if (modal()) draw_approval();
    draw_toast();
}

static int page_index(const char *want) {
    for (int i = 0; i < npg; i++) if (!strcasecmp(pg[i].id, want) || !strcasecmp(pg[i].title, want)) return i;
    char *e;
    long k = strtol(want, &e, 10);
    return *want && !*e && k >= 0 && k < npg ? (int)k : -1;
}

/* Panel events from the agent's stream: notify (webhook toast), goto (rules, webhooks), approvals. */
static void handle_events(void) {
    char *q[EVQ];
    int n;
    pthread_mutex_lock(&lock);
    n = evn;
    memcpy(q, evq, sizeof q[0] * (size_t)n);
    evn = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        const char *s = q[i];
        int t = json_parse(s, (int)strlen(s), tk, MAXTOK);
        if (t < 1) { free(q[i]); continue; }
        char kind[24];
        str(s, t, 0, "kind", kind, sizeof kind);
        int d = json_get(s, tk, t, 0, "data");
        if (!strcmp(kind, "notify")) {
            str(s, t, d, "title", toast.title, sizeof toast.title);
            str(s, t, d, "text", toast.text, sizeof toast.text);
            if (!toast.title[0] && !toast.text[0] && d >= 0 && tk[d].type == J_STR) json_str(s, tk, d, toast.text, sizeof toast.text);
            char lv[16];
            str(s, t, d, "level", lv, sizeof lv);
            toast.col = !strcmp(lv, "error") ? 0xef4444 : !strcmp(lv, "warn") ? 0xf59e0b : !strcmp(lv, "ok") || !strcmp(lv, "success") ? 0x22c55e : 0x60a5fa;
            double secs = num(s, t, d, "seconds", 5);
            toast.until = now_ms() + (long)(clampf(1, (float)secs, 60) * 1000);
            if (!pb_agent_muted) power_wake("notify");
            logmsg("phonebox: notify '%s' '%s'", toast.title, toast.text);
            if (visible()) { draw_toast(); fb_present(fb); }
        } else if (!strcmp(kind, "goto")) {
            char page[32];
            str(s, t, d, "page", page, sizeof page);
            int k = page_index(page);
            logmsg("phonebox: goto '%s' -> %d", page, k);
            if (k >= 0) { cur = k; if (!pb_agent_muted) power_wake("goto"); if (visible()) pb_draw(); }
        } else if (!strncmp(kind, "approval", 8)) want_approvals = 1;
        free(q[i]);
    }
}

/* ================================================================ main-loop API */

int pb_owns(void) { return have_cfg && !panel_has_layout(); } /* a PPJ/1 client's layout wins */
int pb_fd(void) { return notify_r; }

void pb_handle(void) {
    char x[64];
    while (read(notify_r, x, sizeof x) > 0) {}
    pthread_mutex_lock(&lock);
    struct blob c = in_cfg, s = in_state, a = in_art, w = in_wx, wk = in_work, qb = in_quota, pr = in_procs, apb = in_appr;
    memset(&in_appr, 0, sizeof in_appr);
    memset(&in_cfg, 0, sizeof in_cfg); memset(&in_state, 0, sizeof in_state);
    memset(&in_art, 0, sizeof in_art); memset(&in_wx, 0, sizeof in_wx);
    memset(&in_work, 0, sizeof in_work); memset(&in_quota, 0, sizeof in_quota); memset(&in_procs, 0, sizeof in_procs);
    pthread_mutex_unlock(&lock);
    int full = 0, mask = 0;
    if (c.data) {
        int had = have_cfg;
        load_config(c.data);
        full = 1;
        /* first dashboard: show it (not in test mode, where the suites own the screen) */
        if (!had && have_cfg && !pb_agent_muted && hm_owner() == OWN_PAGE && ui_page() != UI_PAGE_PANEL && ui_page() != UI_PAGE_MODEM &&
            gadget_mode() == 'B')
            ui_page_set(UI_PAGE_PANEL);
    }
    if (s.data) {
        load_state(s.data);
        mask |= 1 << T_STAT | 1 << T_GAUGE | 1 << T_CHART | 1 << T_METERS | 1 << T_MEDIA | 1 << T_CLOCK;
    }
    if (w.data) { load_weather(w.data); mask |= 1 << T_CLOCK | 1 << T_FORECAST; }
    if (a.data) {
        free(art_raw.data);
        art_raw = a;              /* kept: decoded by draw_media at its size */
        art_raw_id = (long)art_want;
        art_id_loaded = -1;
        a.data = NULL;
        mask |= 1 << T_MEDIA;
    }
    if (wk.data) { load_work(wk.data); mask |= 1 << T_SESSIONS | 1 << T_GITHUB; }
    if (qb.data) { load_quotas(qb.data); mask |= 1 << T_QUOTAS; }
    if (pr.data) { load_procs(pr.data); mask |= 1 << T_PROCS; }
    if (apb.data) { int had = nap; load_approvals(apb.data); free(apb.data); if ((had || nap) && visible()) full = 1; }
    if (evn) handle_events();
    free(c.data); free(s.data); free(a.data); free(w.data); free(wk.data); free(qb.data); free(pr.data);
    if (have_cfg) standby_tick(); /* also here: with the screen off the 1 s UI tick doesn't run, data does */
    if (full) { if (visible()) pb_draw(); }
    else if (mask) redraw_types(mask);
}

/* Once a second (main loop): the clock, media progress, re-flow after rotation, weather every 15 min. */
void pb_route_tick(void) {
    /* Main loop, every second even with the screen off. Cable back while streaming over Wi-Fi: USB wins. The
     * stream loop checks this between events too, but on a slow Wi-Fi link one event (album art) can take
     * minutes, so cut the socket from here; the loop reconnects over USB. */
    int fd = wifi_stream_fd;
    if (fd >= 0 && route_wifi && adbd_reverse_active(AGENT_PORT) && strcmp(store_get("pb.route", ""), "wifi") &&
        now_ms() - stream_opened_ms > 3000) {
        logmsg("phonebox: cable back, leaving Wi-Fi for USB");
        wifi_stream_fd = -1;
        shutdown(fd, SHUT_RDWR);
    }
}

void pb_tick(void) {
    static int last_land = -1;
    static long wx_at;
    if (!have_cfg) return;
    standby_tick();
    if (toast.until && toast.until <= now_ms()) { toast.until = 0; if (visible() && !gone) { pb_draw(); return; } }
    if (now_ms() - wx_at > 15 * 60 * 1000) { wx_at = now_ms(); wx_want = 1; }
    int procs = 0;
    for (int i = 0; i < pg[cur].n; i++) procs |= pg[cur].w[i].type == T_PROCS;
    want_procs = procs && visible();
    int l = fb && fb->w > fb->h;
    if (l != last_land) { last_land = l; if (visible()) { pb_draw(); return; } }
    redraw_types(1 << T_CLOCK | (S.playing ? 1 << T_MEDIA : 0));
}

/* Touch (logical px). Taps on hit boxes POST; horizontal swipes change the page. */
int pb_touch(const int *ids, const int *xs, const int *ys, int n) {
    static int tid = -1, x0, y0, xl, yl;
    static struct hit pressed;
    int idx = -1;
    for (int i = 0; i < n; i++) if (ids[i] == tid) idx = i;
    if (tid >= 0 && idx < 0) { /* lifted */
        tid = -1;
        int dx = xl - x0, dy = yl - y0, had = pressed_action[0] != 0;
        pressed_action[0] = 0;
        if (abs(dx) > fb->w / 8 && abs(dx) > abs(dy)) pb_page_step(dx < 0 ? 1 : -1);
        else {
            if (had && xl >= pressed.x && xl < pressed.x + pressed.w && yl >= pressed.y && yl < pressed.y + pressed.h) {
                post(pressed.action);
                if (!strncmp(pressed.action, "/api/approvals/", 15)) { approval_done(pressed.action); pb_draw(); return 1; }
            }
            if (had) redraw_types(1 << T_MEDIA | 1 << T_SHORTCUTS);
        }
        return 1;
    }
    if (tid < 0 && n > 0) {
        tid = ids[0]; x0 = xl = xs[0]; y0 = yl = ys[0];
        for (int k = 0; k < nhits; k++)
            if (x0 >= hits[k].x && x0 < hits[k].x + hits[k].w && y0 >= hits[k].y && y0 < hits[k].y + hits[k].h) {
                pressed = hits[k];
                snprintf(pressed_action, sizeof pressed_action, "%s", hits[k].action);
                redraw_types(1 << T_MEDIA | 1 << T_SHORTCUTS);
                break;
            }
        return 1;
    }
    if (idx >= 0) { xl = xs[idx]; yl = ys[idx]; }
    return 1;
}

/* Side buttons / swipes: next or previous phonebox page (wraps). Returns 0 if handled. */
int pb_page_set(int n) {
    if (!pb_owns() || n < 0 || n >= npg) return -1;
    cur = n;
    if (visible()) pb_draw();
    return 0;
}

int pb_page_step(int d) {
    if (!pb_owns() || npg < 1) return 1;
    cur = (cur + d + npg) % npg;
    pb_draw();
    return 0;
}

/* "x y w h action" per tap target on the current page (logical px), for tests. */
int pb_hits(char *b, size_t len) {
    size_t o = 0;
    for (int i = 0; i < nhits && o + 80 < len; i++)
        o += (size_t)snprintf(b + o, len - o, "%d %d %d %d %s\n", hits[i].x, hits[i].y, hits[i].w, hits[i].h, hits[i].action);
    return (int)o;
}

int pb_stats(char *b, size_t len) {
    return snprintf(b, len, "route %s\npaired %s\nstandby %s%s\nconnected %d\nconfig_version %lld\nconfigs %lu\nstates %lu\nposts %lu\npages %d\npage %d\nweather %d\n"
                    "tz_off %ld\nart %ld\nhits %d\n",
                    route_wifi ? "wifi" : "usb", strlen(store_get("pb.fp", "")) == 64 ? "yes" : "no", gone ? "on " : "off ", standby_mode(), connected, cfg_version, n_cfgs, n_states, n_posts, npg, cur, W.ok, tz_off, art_id_loaded, nhits);
}

void pb_init(struct fb *f) {
    fb = f;
    gfx_init();
    int pf[2];
    if (pipe2(pf, O_CLOEXEC | O_NONBLOCK) < 0) return;
    notify_r = pf[0]; notify_w = pf[1];
    pthread_t t;
    pthread_create(&t, NULL, http_thread, NULL); pthread_detach(t);
    pthread_create(&t, NULL, side_thread, NULL); pthread_detach(t);
}
