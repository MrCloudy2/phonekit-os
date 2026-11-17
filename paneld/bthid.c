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

/* ---------------------------------------------------------------- HID report descriptor */

static const unsigned char report_desc[] = {
    /* keyboard, report 1: modifiers, reserved, 6 keys; LEDs out */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    /* mouse, report 2: 5 buttons, X/Y 16-bit relative, wheel, AC pan */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

/* ---------------------------------------------------------------- SDP record */

static unsigned char rec[1024];
static size_t rec_len;

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

static void build_record(void) {
    struct buf w = {rec, 0, sizeof rec};
    size_t top = seq_open(&w), a, b, c;
    attr(&w, 0x0000); de_u32(&w, 0x00010000);
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
    attr(&w, 0x0203); de_u8(&w, 0x00);
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
    rec_len = w.n;
}

/* Answers one SDP request PDU. Every search returns our single record; attribute requests get the whole
 * record (hosts pick what they need). Long answers are split with a 2-byte continuation offset. */
static void sdp_handle(int fd, const unsigned char *q, ssize_t n) {
    if (n < 5) return;
    unsigned pdu = q[0], tid = (unsigned)(q[1] << 8 | q[2]);
    logmsg("bthid: SDP request 0x%02x (%zd bytes)", pdu, n);
    unsigned char out[1024];
    struct buf w = {out, 0, sizeof out};
    if (pdu == 0x02) { /* ServiceSearchRequest -> ServiceSearchResponse */
        put8(&w, 0x03); put16(&w, tid); put16(&w, 9); put16(&w, 1); put16(&w, 1); put32(&w, 0x00010000); put8(&w, 0);
        (void)!send(fd, out, w.n, 0);
        return;
    }
    if (pdu != 0x04 && pdu != 0x06) { /* unsupported: ErrorResponse, invalid request syntax */
        put8(&w, 0x01); put16(&w, tid); put16(&w, 2); put16(&w, 0x0003);
        (void)!send(fd, out, w.n, 0);
        return;
    }
    /* find max byte count and the continuation state at the end of the request */
    const unsigned char *p = q + 5, *end = q + n;
    if (pdu == 0x04) p += 4;                 /* service record handle */
    else {                                   /* service search pattern (data element sequence) */
        if (p >= end) return;
        size_t l = *p == 0x35 ? (size_t)p[1] + 2 : *p == 0x36 ? (size_t)(p[1] << 8 | p[2]) + 3 : 2;
        p += l;
    }
    if (p + 2 > end) return;
    unsigned maxb = (unsigned)(p[0] << 8 | p[1]);
    p += 2;
    if (p < end) { size_t l = *p == 0x35 ? (size_t)p[1] + 2 : *p == 0x36 ? (size_t)(p[1] << 8 | p[2]) + 3 : 2; p += l; } /* attribute id list */
    unsigned off = 0;
    if (p < end && *p == 2 && p + 3 <= end) off = (unsigned)(p[1] << 8 | p[2]);
    /* the full answer: our record (ServiceAttribute) or a sequence holding it (ServiceSearchAttribute) */
    static unsigned char full[1100];
    size_t fl = 0;
    if (pdu == 0x06) { full[0] = 0x36; full[1] = (unsigned char)(rec_len >> 8); full[2] = (unsigned char)rec_len; fl = 3; }
    memcpy(full + fl, rec, rec_len);
    fl += rec_len;
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
    if (n) logmsg("bthid: %zu bonded host(s) loaded", n);
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
        if (st) logmsg("bthid: mgmt command 0x%04x failed, status 0x%02x", op, st);
    } else if (ev != 0x0006 && ev != 0x0008) /* skip new-settings / class-changed chatter */
        logmsg("bthid: mgmt event 0x%04x (%zd bytes)", ev, n - 6);
    if (ev == 0x000F && n >= 6 + 7) { /* User Confirmation Request -> accept ("just works") */
        mgmt(0x001C, p, 7);
        logmsg("bthid: pairing confirmed for %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
    } else if (ev == 0x000E && n >= 6 + 7) { /* PIN Code Request (legacy): 0000 */
        unsigned char r[7 + 1 + 16] = {0};
        memcpy(r, p, 7); r[7] = 4; memcpy(r + 8, "0000", 4);
        mgmt(0x0016, r, sizeof r);
    } else if (ev == 0x0009 && n >= 6 + 26) { /* New Link Key: store_hint + key(25) */
        save_key(p + 1);
        logmsg("bthid: bonded with %02X:%02X:%02X:%02X:%02X:%02X", p[6], p[5], p[4], p[3], p[2], p[1]);
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

static void *bthid_thread(void *v) {
    (void)v;
    while (run && access("/sys/class/bluetooth/hci0", F_OK) != 0) sleep(1);
    sleep(2); /* let the kernel finish the controller setup */
    mg = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    struct sockaddr_hci ma = {AF_BT, 0xffff, HCI_CHANNEL_CONTROL};
    if (mg < 0 || bind(mg, (struct sockaddr *)&ma, sizeof ma) < 0) { logmsg("bthid: mgmt: %s", strerror(errno)); run = 0; return NULL; }
    build_record();
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
    if (sdp < 0 || ctl < 0 || itr < 0) logmsg("bthid: L2CAP listen failed (%s)", strerror(errno));
    logmsg("bthid: ready (SDP, HID control/interrupt listening)");
    snprintf(status_line, sizeof status_line, "ready, not connected");
    int sdpc = -1;
    long disc_until = 0;
    while (run) {
        if (discoverable_req) {
            discoverable_req = 0;
            unsigned char d[3] = {1, 180, 0}; /* general discoverable for 180 s */
            mgmt(0x0006, d, 3);
            disc_until = now_ms() + 180000;
            logmsg("bthid: discoverable for 3 minutes");
        }
        if (disc_until && now_ms() > disc_until) disc_until = 0;
        struct pollfd p[7];
        int np = 0;
        p[np++] = (struct pollfd){.fd = mg, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = sdp, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = ctl, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = itr, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = sdpc, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = ctrl_fd, .events = POLLIN};
        p[np++] = (struct pollfd){.fd = intr_fd, .events = POLLIN};
        if (poll(p, (nfds_t)np, 500) <= 0) continue;
        if (p[0].revents & POLLIN) mgmt_event();
        if (p[1].revents & POLLIN) { int c = accept4(sdp, NULL, NULL, SOCK_CLOEXEC); if (c >= 0) { if (sdpc >= 0) close(sdpc); sdpc = c; } }
        struct sockaddr_l2 ra;
        socklen_t rl = sizeof ra;
        if (p[2].revents & POLLIN) {
            int c = accept4(ctl, (struct sockaddr *)&ra, &rl, SOCK_CLOEXEC);
            if (c >= 0) { if (ctrl_fd >= 0) close(ctrl_fd); ctrl_fd = c; memcpy(host, ra.l2_bdaddr, 6); logmsg("bthid: control channel opened"); }
        }
        rl = sizeof ra;
        if (p[3].revents & POLLIN) {
            int c = accept4(itr, (struct sockaddr *)&ra, &rl, SOCK_CLOEXEC);
            if (c >= 0) {
                logmsg("bthid: interrupt channel opened");
                pthread_mutex_lock(&send_lock);
                if (intr_fd >= 0) close(intr_fd);
                intr_fd = c;
                pthread_mutex_unlock(&send_lock);
                snprintf(status_line, sizeof status_line, "connected to %02X:%02X:%02X:%02X:%02X:%02X",
                         host[5], host[4], host[3], host[2], host[1], host[0]);
                logmsg("bthid: %s", status_line);
            }
        }
        unsigned char q[700];
        if (sdpc >= 0 && p[4].revents) {
            ssize_t n = recv(sdpc, q, sizeof q, 0);
            if (n <= 0) { close(sdpc); sdpc = -1; } else sdp_handle(sdpc, q, n);
        }
        if (ctrl_fd >= 0 && p[5].revents) {
            ssize_t n = recv(ctrl_fd, q, sizeof q, 0);
            if (n <= 0) { close(ctrl_fd); ctrl_fd = -1; }
            else {
                unsigned t = q[0] >> 4;
                logmsg("bthid: control message 0x%02x (%zd bytes)", q[0], n);
                if (t == 0x1 && (q[0] & 0x0f) == 0x05) { close(ctrl_fd); ctrl_fd = -1; } /* virtual cable unplug */
                else if (t == 0x4) { unsigned char h = 0x03; (void)!send(ctrl_fd, &h, 1, 0); } /* GET_REPORT: unsupported */
                else if (t == 0x5 || t == 0x7 || t == 0x9) { unsigned char h = 0x00; (void)!send(ctrl_fd, &h, 1, 0); } /* SET_REPORT/PROTOCOL/IDLE: ok */
            }
        }
        if (intr_fd >= 0 && p[6].revents) {
            ssize_t n = recv(intr_fd, q, sizeof q, MSG_DONTWAIT); /* host output reports (keyboard LEDs): ignored */
            if (n == 0 || (n < 0 && errno != EAGAIN)) {
                pthread_mutex_lock(&send_lock);
                close(intr_fd); intr_fd = -1;
                pthread_mutex_unlock(&send_lock);
                snprintf(status_line, sizeof status_line, "ready, not connected");
                logmsg("bthid: host disconnected");
            }
        }
    }
    pthread_mutex_lock(&send_lock);
    if (intr_fd >= 0) close(intr_fd);
    intr_fd = -1;
    pthread_mutex_unlock(&send_lock);
    if (ctrl_fd >= 0) close(ctrl_fd);
    ctrl_fd = -1;
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
        if (send(intr_fd, r, n, MSG_DONTWAIT) == (ssize_t)n) sent++;
        else { send_fail++; if (send_fail < 5) logmsg("bthid: send: %s", strerror(errno)); }
    }
    pthread_mutex_unlock(&send_lock);
}

void bthid_key(int mods, int code) {
    unsigned char r[10] = {0xA1, 0x01, (unsigned char)mods, 0, (unsigned char)code, 0, 0, 0, 0, 0};
    send_report(r, sizeof r);
}

void bthid_mouse(int buttons, int dx, int dy, int wheel, int pan) {
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
const char *bthid_status(void) {
    static char b[160];
    snprintf(b, sizeof b, "%s (reports sent %lu, failed %lu)", run ? status_line : "off", sent, send_fail);
    return b;
}
