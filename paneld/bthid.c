// Bluetooth HID device (M10c): the phone as a wireless keyboard + mouse, without bluetoothd.
//
//   mgmt (HCI_CHANNEL_CONTROL): power, name "PhoneKit", class keyboard+pointing, SSP "just works", bonding;
//                               link keys are kept in the settings store (bt.keys) and loaded at start
//   SDP server on PSM 1:        one HID record (keyboard report 1 + mouse report 2)
//   HID control (PSM 17) and interrupt (PSM 19) channels: the host connects after pairing; input reports go out on
//                               the interrupt channel.
//
//   ppctl bt pair       discoverable for 3 minutes
//   ppctl bt            status (incl. the connected host)
// KEYBOARD/MOUSE/COMBO (and DECK) send to the Bluetooth host too while one is connected (hid_modes.c).
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AF_BT 31
#define BTPROTO_L2CAP 0
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define SOL_BLUETOOTH 274
#define BT_SECURITY 4

struct sockaddr_hci { unsigned short hci_family, hci_dev, hci_channel; };
struct sockaddr_l2 { unsigned short l2_family; unsigned short l2_psm; unsigned char l2_bdaddr[6]; unsigned short l2_cid; unsigned char l2_bdaddr_type; };
struct bt_security { uint8_t level, key_size; };

static volatile int run, discoverable_req;
static int intr_fd = -1, ctrl_fd = -1;
static unsigned char host[6];
static char status_line[96] = "idle";
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int boot_proto;          /* host chose boot protocol (SET_PROTOCOL 0): mouse reports shrink */
static volatile int connect_req;         /* `bt connect` / start-up: reconnect to the bonded host ourselves */
static volatile int pend_ctrl = -1, pend_intr = -1; /* channels opened by the connect helper, adopted by the loop */

/* The last Bluetooth events, for `ppctl bt log` (the kernel log rolls over quickly). */
#define LOGN 48
static char ring[LOGN][140];
static unsigned ring_n;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static void blog(const char *fmt, ...) {
    char m[110];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    logmsg("bthid: %s", m);
    pthread_mutex_lock(&ring_lock);
    snprintf(ring[ring_n++ % LOGN], sizeof ring[0], "[%6ld.%03ld] %s", now_ms() / 1000, now_ms() % 1000, m);
    pthread_mutex_unlock(&ring_lock);
}
static void hexs(char *o, size_t ol, const unsigned char *b, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 4 < ol; i++) k += (size_t)snprintf(o + k, ol - k, "%02x ", b[i]);
    if (n && k) o[k - 1] = 0; else o[0] = 0;
}

/* ---------------------------------------------------------------- HID report descriptor */

static const unsigned char report_desc[] = {
    /* keyboard, report 1: modifiers, reserved, 6 keys; LEDs out */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xE7, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xE7, 0x81, 0x00, /* keys 0..0xE7 (F13-F24) */
    0xC0,
    /* mouse, report 2: 5 buttons, X/Y 16-bit relative, wheel, AC pan */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
    /* consumer control (media keys), report 3: one 16-bit usage */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};

/* ---------------------------------------------------------------- SDP records */
// Two records: HID (0x10000) and Device ID (0x10001, PnP information). Searches return only the records holding
// every UUID of the pattern, and attribute requests only the attributes asked for (hosts look up the Device ID
// record separately and must not get HID attributes in its place).

#define NREC 2
static unsigned char rec[NREC][512];
static size_t rec_len[NREC];
static const unsigned rec_handle[NREC] = {0x00010000, 0x00010001};

struct buf { unsigned char *b; size_t n, cap; };
static void put8(struct buf *w, unsigned v) { if (w->n < w->cap) w->b[w->n++] = (unsigned char)v; }
static void put16(struct buf *w, unsigned v) { put8(w, v >> 8); put8(w, v); }
static void put32(struct buf *w, unsigned v) { put16(w, v >> 16); put16(w, v); }
static void de_u8(struct buf *w, unsigned v) { put8(w, 0x08); put8(w, v); }
static void de_u16(struct buf *w, unsigned v) { put8(w, 0x09); put16(w, v); }
static void de_u32(struct buf *w, unsigned v) { put8(w, 0x0A); put32(w, v); }
static void de_uuid(struct buf *w, unsigned v) { put8(w, 0x19); put16(w, v); }
static void de_bool(struct buf *w, int v) { put8(w, 0x28); put8(w, v ? 1 : 0); }
static void de_text(struct buf *w, const void *s, size_t l) { put8(w, 0x25); put8(w, (unsigned)l); for (size_t i = 0; i < l; i++) put8(w, ((const unsigned char *)s)[i]); }
/* sequences: open with a 16-bit length placeholder, close fills it in */
static size_t seq_open(struct buf *w) { put8(w, 0x36); size_t at = w->n; put16(w, 0); return at; }
static void seq_close(struct buf *w, size_t at) { size_t l = w->n - at - 2; w->b[at] = (unsigned char)(l >> 8); w->b[at + 1] = (unsigned char)l; }
static void attr(struct buf *w, unsigned id) { de_u16(w, id); }

static void build_records(void) {
    struct buf w = {rec[0], 0, sizeof rec[0]};
    size_t top = seq_open(&w), a, b, c;
    attr(&w, 0x0000); de_u32(&w, rec_handle[0]);
    attr(&w, 0x0001); a = seq_open(&w); de_uuid(&w, 0x1124); seq_close(&w, a);
    attr(&w, 0x0004); a = seq_open(&w);
        b = seq_open(&w); de_uuid(&w, 0x0100); de_u16(&w, 0x0011); seq_close(&w, b);
        b = seq_open(&w); de_uuid(&w, 0x0011); seq_close(&w, b);
    seq_close(&w, a);
    attr(&w, 0x0005); a = seq_open(&w); de_uuid(&w, 0x1002); seq_close(&w, a);
    attr(&w, 0x0006); a = seq_open(&w); de_u16(&w, 0x656e); de_u16(&w, 0x006a); de_u16(&w, 0x0100); seq_close(&w, a);
    attr(&w, 0x0009); a = seq_open(&w); b = seq_open(&w); de_uuid(&w, 0x1124); de_u16(&w, 0x0101); seq_close(&w, b); seq_close(&w, a);
    attr(&w, 0x000d); a = seq_open(&w); b = seq_open(&w);
        c = seq_open(&w); de_uuid(&w, 0x0100); de_u16(&w, 0x0013); seq_close(&w, c);
        c = seq_open(&w); de_uuid(&w, 0x0011); seq_close(&w, c);
    seq_close(&w, b); seq_close(&w, a);
    attr(&w, 0x0100); de_text(&w, "PhoneKit", 8);
    attr(&w, 0x0101); de_text(&w, "Keyboard and touchpad", 21);
    attr(&w, 0x0102); de_text(&w, "PhoneKit", 8);
    attr(&w, 0x0201); de_u16(&w, 0x0111);
    attr(&w, 0x0202); de_u8(&w, 0xC0);   /* combo keyboard + pointing */
    attr(&w, 0x0203); de_u8(&w, 33);     /* country code: US layout (Android 14+ picks the keyboard layout from it) */
    attr(&w, 0x0204); de_bool(&w, 1);    /* virtual cable */
    attr(&w, 0x0205); de_bool(&w, 1);    /* reconnect initiate */
    attr(&w, 0x0206); a = seq_open(&w); b = seq_open(&w); de_u8(&w, 0x22); de_text(&w, report_desc, sizeof report_desc); seq_close(&w, b); seq_close(&w, a);
    attr(&w, 0x0207); a = seq_open(&w); b = seq_open(&w); de_u16(&w, 0x0409); de_u16(&w, 0x0100); seq_close(&w, b); seq_close(&w, a);
    attr(&w, 0x0209); de_bool(&w, 1);
    attr(&w, 0x020a); de_bool(&w, 1);
    attr(&w, 0x020b); de_u16(&w, 0x0100);
    attr(&w, 0x020c); de_u16(&w, 0x0c80);
    attr(&w, 0x020d); de_bool(&w, 0);
    attr(&w, 0x020e); de_bool(&w, 1);    /* boot device */
    attr(&w, 0x020f); de_u16(&w, 0x0640);
    attr(&w, 0x0210); de_u16(&w, 0x0320);
    seq_close(&w, top);
    rec_len[0] = w.n;

    /* Device ID: Linux Foundation vendor id (USB source), product "PhoneKit HID" */
    w = (struct buf){rec[1], 0, sizeof rec[1]};
    top = seq_open(&w);
    attr(&w, 0x0000); de_u32(&w, rec_handle[1]);
    attr(&w, 0x0001); a = seq_open(&w); de_uuid(&w, 0x1200); seq_close(&w, a);
    attr(&w, 0x0005); a = seq_open(&w); de_uuid(&w, 0x1002); seq_close(&w, a);
    attr(&w, 0x0009); a = seq_open(&w); b = seq_open(&w); de_uuid(&w, 0x1200); de_u16(&w, 0x0103); seq_close(&w, b); seq_close(&w, a);
    attr(&w, 0x0200); de_u16(&w, 0x0103); /* specification id */
    attr(&w, 0x0201); de_u16(&w, 0x1d6b); /* vendor id */
    attr(&w, 0x0202); de_u16(&w, 0x0246); /* product id */
    attr(&w, 0x0203); de_u16(&w, 0x0100); /* version */
    attr(&w, 0x0204); de_bool(&w, 1);     /* primary record */
    attr(&w, 0x0205); de_u16(&w, 0x0002); /* vendor id source: USB-IF */
    seq_close(&w, top);
    rec_len[1] = w.n;
}

/* Size of the data element at p (header + data), 0 if malformed or longer than n. */
static size_t de_size(const unsigned char *p, size_t n) {
    if (!n) return 0;
    unsigned t = p[0] >> 3, si = p[0] & 7;
    size_t h = 1, d;
    if (si < 5) d = t == 0 ? 0 : (size_t)1 << si;
    else if (si == 5) { if (n < 2) return 0; d = p[1]; h = 2; }
    else if (si == 6) { if (n < 3) return 0; d = (size_t)(p[1] << 8 | p[2]); h = 3; }
    else { if (n < 5) return 0; d = (size_t)p[1] << 24 | (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4]; h = 5; }
    return h + d <= n ? h + d : 0;
}
static size_t de_hdr(const unsigned char *p) { unsigned si = p[0] & 7; return si == 5 ? 2 : si == 6 ? 3 : si == 7 ? 5 : 1; }

/* 16-bit form of a UUID element (16, 32 or 128 bit on the Bluetooth base), or -1. */
static long uuid16(const unsigned char *p, size_t sz) {
    if (p[0] >> 3 != 3) return -1;
    if (sz == 3) return p[1] << 8 | p[2];
    if (sz == 5) return p[3] << 8 | p[4];
    if (sz == 17) return p[3] << 8 | p[4]; /* base 0000xxxx-0000-1000-8000-00805F9B34FB */
    return -1;
}

static int has_uuid(const unsigned char *p, size_t n, long u) {
    while (n) {
        size_t sz = de_size(p, n);
        if (!sz) return 0;
        unsigned t = p[0] >> 3;
        if (t == 3 && uuid16(p, sz) == u) return 1;
        if ((t == 6 || t == 7) && has_uuid(p + de_hdr(p), sz - de_hdr(p), u)) return 1;
        p += sz; n -= sz;
    }
    return 0;
}

/* Does record r hold every UUID in the search pattern (a sequence at p)? */
static int rec_matches(int r, const unsigned char *pat, size_t psz) {
    const unsigned char *p = pat + de_hdr(pat);
    size_t n = psz - de_hdr(pat);
    int any = 0;
    while (n) {
        size_t sz = de_size(p, n);
        if (!sz) return 0;
        long u = uuid16(p, sz);
        if (u < 0 || !has_uuid(rec[r], rec_len[r], u)) return 0;
        any = 1;
        p += sz; n -= sz;
    }
    return any;
}

/* Is attribute id in the attribute id list (a sequence of uint16 ids and uint32 hi/lo ranges)? */
static int attr_wanted(const unsigned char *ids, size_t isz, unsigned id) {
    const unsigned char *p = ids + de_hdr(ids);
    size_t n = isz - de_hdr(ids);
    while (n) {
        size_t sz = de_size(p, n);
        if (!sz) return 0;
        if (p[0] == 0x09 && sz == 3 && (unsigned)(p[1] << 8 | p[2]) == id) return 1;
        if (p[0] == 0x0A && sz == 5 && id >= (unsigned)(p[1] << 8 | p[2]) && id <= (unsigned)(p[3] << 8 | p[4])) return 1;
        p += sz; n -= sz;
    }
    return 0;
}

/* Appends record r reduced to the wanted attributes, as one sequence. */
static void put_attrs(struct buf *w, int r, const unsigned char *ids, size_t isz) {
    size_t at = seq_open(w);
    const unsigned char *p = rec[r] + 3;
    size_t n = rec_len[r] - 3;
    while (n) {
        size_t a = de_size(p, n);
        if (!a || a != 3) break;
        size_t v = de_size(p + a, n - a);
        if (!v) break;
        if (attr_wanted(ids, isz, (unsigned)(p[1] << 8 | p[2])))
            for (size_t i = 0; i < a + v; i++) put8(w, p[i]);
        p += a + v; n -= a + v;
    }
    seq_close(w, at);
}

/* Answers one SDP request PDU. Long answers are split with a 2-byte continuation offset into the full answer
 * (which is rebuilt identically for the follow-up request). */
static void sdp_handle(int fd, const unsigned char *q, ssize_t n) {
    if (n < 5) return;
    unsigned pdu = q[0], tid = (unsigned)(q[1] << 8 | q[2]);
    const unsigned char *p = q + 5, *end = q + n;
    unsigned char out[1100];
    struct buf w = {out, 0, sizeof out};
    static unsigned char full[1100];
    struct buf f = {full, 0, sizeof full};
    const unsigned char *pat = NULL, *ids = NULL;
    size_t psz = 0, isz = 0;
    unsigned handle = 0, maxb = 0;
    if (pdu == 0x02 || pdu == 0x06) { pat = p; psz = de_size(p, (size_t)(end - p)); p += psz; }
    else if (pdu == 0x04 && p + 4 <= end) { handle = (unsigned)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); p += 4; }
    if ((pdu != 0x02 && pdu != 0x04 && pdu != 0x06) || (pat && (!psz || pat[0] >> 3 != 6)) || p + 2 > end) {
        blog("SDP: unsupported/malformed request 0x%02x (%zd bytes)", pdu, n);
        put8(&w, 0x01); put16(&w, tid); put16(&w, 2); put16(&w, 0x0003); /* invalid request syntax */
        (void)!send(fd, out, w.n, 0);
        return;
    }
    maxb = (unsigned)(p[0] << 8 | p[1]);
    p += 2;
    if (pdu == 0x02) { /* ServiceSearchRequest -> ServiceSearchResponse (handles; never needs continuation) */
        unsigned hs[NREC], nh = 0;
        for (int r = 0; r < NREC; r++) if (rec_matches(r, pat, psz) && nh < maxb) hs[nh++] = rec_handle[r];
        put8(&w, 0x03); put16(&w, tid); put16(&w, 4 + 4 * nh + 1); put16(&w, nh); put16(&w, nh);
        for (unsigned i = 0; i < nh; i++) put32(&w, hs[i]);
        put8(&w, 0);
        (void)!send(fd, out, w.n, 0);
        blog("SDP search: %u record(s)", nh);
        return;
    }
    if (p < end) { ids = p; isz = de_size(p, (size_t)(end - p)); p += isz; }
    if (!ids || !isz || ids[0] >> 3 != 6) {
        put8(&w, 0x01); put16(&w, tid); put16(&w, 2); put16(&w, 0x0003);
        (void)!send(fd, out, w.n, 0);
        return;
    }
    unsigned off = 0;
    if (p < end && *p == 2 && p + 3 <= end) off = (unsigned)(p[1] << 8 | p[2]);
    int nrec = 0;
    if (pdu == 0x04) { /* ServiceAttributeRequest: one record's attributes */
        int r = -1;
        for (int i = 0; i < NREC; i++) if (rec_handle[i] == handle) r = i;
        if (r < 0) {
            put8(&w, 0x01); put16(&w, tid); put16(&w, 2); put16(&w, 0x0002); /* invalid service record handle */
            (void)!send(fd, out, w.n, 0);
            return;
        }
        put_attrs(&f, r, ids, isz);
        nrec = 1;
    } else { /* ServiceSearchAttributeRequest: a sequence of attribute lists, one per matching record */
        size_t at = seq_open(&f);
        for (int r = 0; r < NREC; r++) if (rec_matches(r, pat, psz)) { put_attrs(&f, r, ids, isz); nrec++; }
        seq_close(&f, at);
    }
    size_t fl = f.n;
    if (maxb < 7) maxb = 7;
    if (maxb > 600) maxb = 600;
    if (off > fl) off = (unsigned)fl;
    size_t chunk = fl - off < maxb ? fl - off : maxb;
    int more = off + chunk < fl;
    put8(&w, pdu == 0x04 ? 0x05 : 0x07); put16(&w, tid); put16(&w, (unsigned)(2 + chunk + (more ? 3 : 1)));
    put16(&w, (unsigned)chunk);
    for (size_t i = 0; i < chunk; i++) put8(&w, full[off + i]);
    if (more) { put8(&w, 2); put16(&w, (unsigned)(off + chunk)); } else put8(&w, 0);
    (void)!send(fd, out, w.n, 0);
    if (!off) blog("SDP %s: %d record(s), %zu bytes", pdu == 0x04 ? "attributes" : "search+attributes", nrec, fl);
}

/* ---------------------------------------------------------------- mgmt */

static int mg = -1;
static void mgmt(unsigned op, const void *par, unsigned len) {
    unsigned char b[600];
    b[0] = (unsigned char)op; b[1] = (unsigned char)(op >> 8); b[2] = 0; b[3] = 0; /* index 0 = hci0 */
    b[4] = (unsigned char)len; b[5] = (unsigned char)(len >> 8);
    memcpy(b + 6, par, len);
    (void)!write(mg, b, 6 + len);
    usleep(30000);
}
static void mgmt_u8(unsigned op, int v) { unsigned char x = (unsigned char)v; mgmt(op, &x, 1); }

static void load_keys(void) {
    const char *hex = store_get("bt.keys", "");
    size_t n = strlen(hex) / 50; /* 25 bytes per key */
    if (n > 8) n = 8;
    unsigned char b[3 + 8 * 25];
    b[0] = 0; b[1] = (unsigned char)n; b[2] = 0;
    for (size_t i = 0; i < n * 25; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[3 + i] = (unsigned char)v; }
    mgmt(0x0012, b, (unsigned)(3 + n * 25));
    if (n) blog("%zu bonded host(s) loaded", n);
}

static void save_key(const unsigned char *k25) {
    char hex[8 * 50 + 1] = "";
    const char *cur = store_get("bt.keys", "");
    /* drop an older key for the same address, keep at most 7 others */
    size_t n = strlen(cur) / 50, o = 0;
    char mine[13];
    for (int i = 0; i < 6; i++) sprintf(mine + 2 * i, "%02x", k25[i]);
    for (size_t i = 0; i < n && o < 7 * 50; i++)
        if (strncasecmp(cur + 50 * i, mine, 12)) { memcpy(hex + o, cur + 50 * i, 50); o += 50; }
    for (int i = 0; i < 25; i++) sprintf(hex + o + 2 * i, "%02x", k25[i]);
    hex[o + 50] = 0;
    store_set("bt.keys", hex);
}

static void mgmt_event(void) {
    unsigned char b[512];
    ssize_t n = read(mg, b, sizeof b);
    if (n < 6) return;
    unsigned ev = (unsigned)(b[0] | b[1] << 8);
    const unsigned char *p = b + 6;
    if (ev == 0x0001 || ev == 0x0002) { /* command complete / status: log failures */
        unsigned op = n >= 8 ? (unsigned)(p[0] | p[1] << 8) : 0, st = n >= 9 ? p[2] : 0;
        if (st) blog("mgmt command 0x%04x failed, status 0x%02x", op, st);
    } else if (ev == 0x000B && n >= 6 + 7) {
        blog("link up with %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
    } else if (ev == 0x000C && n >= 6 + 8) { /* reason: 1 timeout, 2 local, 3 remote, 4 auth failure */
        static const char *why[] = {"unspecified", "link timeout", "closed here", "closed by the host", "authentication failure", "suspended"};
        blog("link down with %02X:%02X:%02X:%02X:%02X:%02X (%s)", p[5], p[4], p[3], p[2], p[1], p[0], p[7] < 6 ? why[p[7]] : "?");
    } else if (ev == 0x0011 && n >= 6 + 8) {
        blog("authentication failed with %02X:%02X:%02X:%02X:%02X:%02X, status 0x%02x", p[5], p[4], p[3], p[2], p[1], p[0], p[7]);
    } else if (ev != 0x0006 && ev != 0x0008 && ev != 0x000F && ev != 0x0009) /* skip new-settings / class chatter */
        blog("mgmt event 0x%04x (%zd bytes)", ev, n - 6);
    if (ev == 0x000F && n >= 6 + 7) { /* User Confirmation Request -> accept ("just works") */
        mgmt(0x001C, p, 7);
        blog("pairing confirmed for %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
    } else if (ev == 0x000E && n >= 6 + 7) { /* PIN Code Request (legacy): 0000 */
        unsigned char r[7 + 1 + 16] = {0};
        memcpy(r, p, 7); r[7] = 4; memcpy(r + 8, "0000", 4);
        mgmt(0x0016, r, sizeof r);
    } else if (ev == 0x0009 && n >= 6 + 26) { /* New Link Key: store_hint + key(25) */
        save_key(p + 1);
        blog("bonded with %02X:%02X:%02X:%02X:%02X:%02X", p[6], p[5], p[4], p[3], p[2], p[1]);
    }
}

/* ---------------------------------------------------------------- L2CAP */

static int l2_listen(unsigned psm) {
    int s = socket(AF_BT, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (s < 0) return -1;
    struct sockaddr_l2 a = {.l2_family = AF_BT, .l2_psm = (unsigned short)psm};
    struct bt_security sec = {1, 0}; /* low: the host decides; HID hosts pair before connecting */
    setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof sec);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 2) < 0) { close(s); return -1; }
    return s;
}

/* Opens an L2CAP channel to the host (encrypted with the stored link key); -1 after 10 s. */
static int l2_connect(const unsigned char *addr, unsigned psm) {
    int s = socket(AF_BT, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (s < 0) return -1;
    struct bt_security sec = {2, 0}; /* medium: authenticated by the bond, encrypted */
    setsockopt(s, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof sec);
    struct sockaddr_l2 a = {.l2_family = AF_BT, .l2_psm = (unsigned short)psm};
    memcpy(a.l2_bdaddr, addr, 6);
    fcntl(s, F_SETFL, O_NONBLOCK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(s); return -1; }
    struct pollfd p = {.fd = s, .events = POLLOUT};
    int e = 0;
    socklen_t el = sizeof e;
    if (poll(&p, 1, 10000) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
        if (e) errno = e; else errno = ETIMEDOUT;
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, 0);
    return s;
}

/* Reconnect helper (HIDReconnectInitiate): control then interrupt channel to the newest bonded host. Runs in its
 * own thread so pairing prompts keep being answered meanwhile. */
static void *connect_thread(void *v) {
    (void)v;
    const char *hex = store_get("bt.keys", "");
    size_t n = strlen(hex) / 50;
    if (!n) { blog("connect: no bonded host (pair first)"); return NULL; }
    unsigned char a[6];
    for (int i = 0; i < 6; i++) { unsigned x; sscanf(hex + 50 * (n - 1) + 2 * i, "%2x", &x); a[i] = (unsigned char)x; }
    blog("connect: calling %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    int c = -1;
    for (int t = 0; t < 5 && run; t++) { /* right after power-on the radio isn't ready yet: EHOSTUNREACH */
        c = l2_connect(a, 17);
        if (c >= 0 || errno != EHOSTUNREACH) break;
        sleep(2);
    }
    if (c < 0) { blog("connect: control channel: %s (is the host's Bluetooth on and in range?)", strerror(errno)); return NULL; }
    int i = l2_connect(a, 19);
    if (i < 0) { blog("connect: interrupt channel: %s", strerror(errno)); close(c); return NULL; }
    memcpy(host, a, 6);
    pend_ctrl = c;
    pend_intr = i;
    return NULL;
}

static void set_connected(int c, int i) {
    if (ctrl_fd >= 0) close(ctrl_fd);
    ctrl_fd = c;
    pthread_mutex_lock(&send_lock);
    if (intr_fd >= 0) close(intr_fd);
    intr_fd = i;
    pthread_mutex_unlock(&send_lock);
    boot_proto = 0;
    snprintf(status_line, sizeof status_line, "connected to %02X:%02X:%02X:%02X:%02X:%02X",
             host[5], host[4], host[3], host[2], host[1], host[0]);
    blog("%s", status_line);
}

static void set_disconnected(const char *why) {
    pthread_mutex_lock(&send_lock);
    if (intr_fd >= 0) close(intr_fd);
    intr_fd = -1;
    pthread_mutex_unlock(&send_lock);
    if (ctrl_fd >= 0) close(ctrl_fd);
    ctrl_fd = -1;
    snprintf(status_line, sizeof status_line, "ready, not connected");
    blog("host disconnected (%s)", why);
}

/* HID control channel (HIDP): handshake / GET_REPORT / SET_* / protocol / idle / virtual cable unplug. */
static void control_message(const unsigned char *q, ssize_t n) {
    char hx[64];
    hexs(hx, sizeof hx, q, (size_t)(n > 16 ? 16 : n));
    blog("control: %s", hx);
    unsigned t = q[0] >> 4, prm = q[0] & 0x0f;
    unsigned char r[12];
    switch (t) {
    case 0x1: /* HID_CONTROL */
        if (prm == 0x05) set_disconnected("virtual cable unplugged");
        break;
    case 0x4: { /* GET_REPORT: type in bits 0-1, report id follows */
        unsigned type = prm & 3, id = n > 1 ? q[1] : 0;
        if (type == 1 && id == 1) { memset(r, 0, sizeof r); r[0] = 0xA1; r[1] = 1; (void)!send(ctrl_fd, r, 10, 0); }
        else if (type == 1 && id == 2) { memset(r, 0, sizeof r); r[0] = 0xA1; r[1] = 2; (void)!send(ctrl_fd, r, boot_proto ? 5 : 9, 0); }
        else { r[0] = 0x02; (void)!send(ctrl_fd, r, 1, 0); } /* handshake: invalid report id */
        break;
    }
    case 0x5: r[0] = 0x00; (void)!send(ctrl_fd, r, 1, 0); break; /* SET_REPORT (keyboard LEDs): ok */
    case 0x6: r[0] = 0xA0; r[1] = boot_proto ? 0 : 1; (void)!send(ctrl_fd, r, 2, 0); break; /* GET_PROTOCOL */
    case 0x7: boot_proto = !(prm & 1); r[0] = 0x00; (void)!send(ctrl_fd, r, 1, 0); /* SET_PROTOCOL */
        blog("host chose %s protocol", boot_proto ? "boot" : "report");
        break;
    case 0x8: r[0] = 0xA0; r[1] = 0; (void)!send(ctrl_fd, r, 2, 0); break; /* GET_IDLE */
    case 0x9: r[0] = 0x00; (void)!send(ctrl_fd, r, 1, 0); break; /* SET_IDLE */
    case 0x0: break; /* handshake from the host */
    default: r[0] = 0x03; (void)!send(ctrl_fd, r, 1, 0); break; /* unsupported request */
    }
}

static void *bthid_thread(void *v) {
    (void)v;
    while (run && access("/sys/class/bluetooth/hci0", F_OK) != 0) sleep(1);
    sleep(2); /* let the kernel finish the controller setup */
    mg = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    struct sockaddr_hci ma = {AF_BT, 0xffff, HCI_CHANNEL_CONTROL};
    if (mg < 0 || bind(mg, (struct sockaddr *)&ma, sizeof ma) < 0) { blog("mgmt: %s", strerror(errno)); run = 0; return NULL; }
    build_records();
    mgmt_u8(0x0005, 0);              /* powered off while configuring */
    mgmt_u8(0x000B, 1);              /* SSP */
    mgmt_u8(0x0009, 1);              /* bondable */
    mgmt_u8(0x0007, 1);              /* connectable */
    mgmt_u8(0x0018, 3);              /* IO capability: NoInputNoOutput ("just works") */
    unsigned char cls[2] = {0x05, 0xC0}; /* major peripheral, minor keyboard+pointing */
    mgmt(0x000E, cls, 2);
    unsigned char name[249 + 11] = {0};
    memcpy(name, "PhoneKit", 8); memcpy(name + 249, "PhoneKit", 8);
    mgmt(0x000F, name, sizeof name);
    load_keys();
    mgmt_u8(0x0005, 1);              /* power on */
    int sdp = l2_listen(1), ctl = l2_listen(17), itr = l2_listen(19);
    if (sdp < 0 || ctl < 0 || itr < 0) blog("L2CAP listen failed (%s)", strerror(errno));
    blog("ready (SDP, HID control/interrupt listening)");
    snprintf(status_line, sizeof status_line, "ready, not connected");
    if (store_get("bt.keys", "")[0]) connect_req = 1; /* bonded: call the host back, as a keyboard does */
    int sdpc = -1, half_ctrl = -1; /* control channel accepted, interrupt not yet */
    long disc_until = 0;
    while (run) {
        if (discoverable_req) {
            discoverable_req = 0;
            unsigned char d[3] = {1, 180, 0}; /* general discoverable for 180 s */
            mgmt(0x0006, d, 3);
            disc_until = now_ms() + 180000;
            blog("discoverable for 3 minutes");
        }
        if (disc_until && now_ms() > disc_until) disc_until = 0;
        if (connect_req && intr_fd < 0) {
            connect_req = 0;
            pthread_t t;
            if (!pthread_create(&t, NULL, connect_thread, NULL)) pthread_detach(t);
        }
        if (pend_intr >= 0) { int c = pend_ctrl, i = pend_intr; pend_ctrl = pend_intr = -1; set_connected(c, i); }
        struct pollfd p[8];
        int np = 0;
        p[np++] = (struct pollfd){.fd = mg, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = sdp, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = ctl, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = itr, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = sdpc, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = ctrl_fd, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = intr_fd, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = half_ctrl, .events = POLLIN};
        if (poll(p, (nfds_t)np, 500) <= 0) continue;
        if (p[0].revents & POLLIN) mgmt_event();
        if (p[1].revents & POLLIN) { int c = accept4(sdp, NULL, NULL, SOCK_CLOEXEC); if (c >= 0) { if (sdpc >= 0) close(sdpc); sdpc = c; } }
        struct sockaddr_l2 ra;
        socklen_t rl = sizeof ra;
        if (p[2].revents & POLLIN) {
            int c = accept4(ctl, (struct sockaddr *)&ra, &rl, SOCK_CLOEXEC);
            if (c >= 0) { if (half_ctrl >= 0) close(half_ctrl); half_ctrl = c; memcpy(host, ra.l2_bdaddr, 6); blog("host opened the control channel"); }
        }
        rl = sizeof ra;
        if (p[3].revents & POLLIN) {
            int c = accept4(itr, (struct sockaddr *)&ra, &rl, SOCK_CLOEXEC);
            if (c >= 0) {
                if (half_ctrl >= 0) { set_connected(half_ctrl, c); half_ctrl = -1; }
                else { blog("interrupt channel without a control channel: refused"); close(c); }
            }
        }
        unsigned char q[700];
        if (sdpc >= 0 && p[4].revents) {
            ssize_t n = recv(sdpc, q, sizeof q, 0);
            if (n <= 0) { close(sdpc); sdpc = -1; } else sdp_handle(sdpc, q, n);
        }
        if (ctrl_fd >= 0 && p[5].revents) {
            ssize_t n = recv(ctrl_fd, q, sizeof q, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN)) set_disconnected("control channel closed");
            else if (n > 0) control_message(q, n);
        }
        if (intr_fd >= 0 && p[6].revents) {
            ssize_t n = recv(intr_fd, q, sizeof q, MSG_DONTWAIT); /* host output reports (keyboard LEDs) */
            if (n == 0 || (n < 0 && errno != EAGAIN)) set_disconnected("interrupt channel closed");
            else if (n > 0) { char hx[48]; hexs(hx, sizeof hx, q, (size_t)(n > 12 ? 12 : n)); blog("interrupt in: %s", hx); }
        }
        if (half_ctrl >= 0 && p[7].revents) { /* control channel messages before the interrupt channel is up */
            ssize_t n = recv(half_ctrl, q, sizeof q, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN)) { close(half_ctrl); half_ctrl = -1; }
            else if (n > 0) { int keep = ctrl_fd; ctrl_fd = half_ctrl; control_message(q, n); ctrl_fd = keep; }
        }
    }
    if (half_ctrl >= 0) close(half_ctrl);
    set_disconnected("Bluetooth off");
    close(sdp); close(ctl); close(itr); close(mg);
    mg = -1;
    return NULL;
}

/* ---------------------------------------------------------------- API */

int bthid_connected(void) { return intr_fd >= 0; }

static unsigned long sent, send_fail;
static void send_report(const unsigned char *r, size_t n) {
    pthread_mutex_lock(&send_lock);
    if (intr_fd >= 0) {
        if (send(intr_fd, r, n, MSG_DONTWAIT) == (ssize_t)n) {
            if (sent++ < 4) { char hx[40]; hexs(hx, sizeof hx, r, n); blog("report out: %s", hx); }
        } else { send_fail++; if (send_fail < 5) blog("send: %s", strerror(errno)); }
    }
    pthread_mutex_unlock(&send_lock);
}

void bthid_key(int mods, int code) {
    unsigned char r[10] = {0xA1, 0x01, (unsigned char)mods, 0, (unsigned char)code, 0, 0, 0, 0, 0};
    send_report(r, sizeof r);
}

void bthid_keys(int mods, const unsigned char keys[6]) {
    unsigned char r[10] = {0xA1, 0x01, (unsigned char)mods, 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]};
    send_report(r, sizeof r);
}

void bthid_consumer(int usage) {
    unsigned char r[4] = {0xA1, 0x03, (unsigned char)usage, (unsigned char)(usage >> 8)};
    send_report(r, sizeof r);
}

void bthid_mouse(int buttons, int dx, int dy, int wheel, int pan) {
    if (boot_proto) { /* boot mouse: buttons, 8-bit X/Y */
        dx = dx > 127 ? 127 : dx < -127 ? -127 : dx;
        dy = dy > 127 ? 127 : dy < -127 ? -127 : dy;
        unsigned char r[5] = {0xA1, 0x02, (unsigned char)(buttons & 7), (unsigned char)(int8_t)dx, (unsigned char)(int8_t)dy};
        send_report(r, sizeof r);
        return;
    }
    dx = dx > 32767 ? 32767 : dx < -32767 ? -32767 : dx;
    dy = dy > 32767 ? 32767 : dy < -32767 ? -32767 : dy;
    unsigned char r[9] = {0xA1, 0x02, (unsigned char)buttons, (unsigned char)dx, (unsigned char)(dx >> 8),
                          (unsigned char)dy, (unsigned char)(dy >> 8), (unsigned char)(int8_t)wheel, (unsigned char)(int8_t)pan};
    send_report(r, sizeof r);
}

void bthid_set(int on) {
    if (on && !run) {
        run = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, bthid_thread, NULL)) run = 0;
        else pthread_detach(t);
    } else if (!on) run = 0;
}

void bthid_pair(void) { discoverable_req = 1; }
void bthid_connect(void) { connect_req = 1; }
void bthid_disconnect(void) {
    if (ctrl_fd >= 0) { unsigned char u = 0x15; (void)!send(ctrl_fd, &u, 1, 0); } /* virtual cable unplug */
}

int bthid_log(char *out, size_t len) {
    size_t o = 0;
    pthread_mutex_lock(&ring_lock);
    unsigned first = ring_n > LOGN ? ring_n - LOGN : 0;
    for (unsigned i = first; i < ring_n && o + 130 < len; i++) o += (size_t)snprintf(out + o, len - o, "%s\n", ring[i % LOGN]);
    pthread_mutex_unlock(&ring_lock);
    if (!o) o = (size_t)snprintf(out, len, "(no Bluetooth events yet)\n");
    return (int)o;
}

const char *bthid_status(void) {
    static char b[200];
    snprintf(b, sizeof b, "%s (reports sent %lu, failed %lu%s)", run ? status_line : "off", sent, send_fail,
             boot_proto ? ", boot protocol" : "");
    return b;
}
