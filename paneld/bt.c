// Bluetooth (M10c): bridges Samsung's SCSC raw H4 device (/dev/scsc_h4_0, meant for Android's userspace stack) to the
// kernel's virtual HCI (/dev/vhci), so the Linux Bluetooth core gets hci0. Off unless enabled (opening scsc_h4 starts
// the chip's Bluetooth service).
//
//   ppctl bt on|off      enable/disable (persists)
//   ppctl bt wired-off on|off  keep Bluetooth off while a computer is on the USB cable (default on)
//   ppctl bt forget      drop the paired host
//   ppctl bt             status
//   ppctl bt scan        power hci0 up and run an 8 s inquiry (classic discovery) via a raw HCI socket
//   ppctl bt pair        discoverable for 3 minutes (pair from the host)
//   ppctl bt connect     call the bonded host back (also done at start); bt disconnect: virtual cable unplug
//   ppctl bt type TEXT   type over Bluetooth; bt key CODE [MODS], bt mouse DX DY: single raw reports
//   ppctl bt log         the last Bluetooth events (pairing, SDP, channels, control messages)
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define H4 "/dev/scsc_h4_0"
#define VHCI "/dev/vhci"
static volatile int running, want;
static unsigned long pk_up, pk_down, pk_drop; /* controller->host, host->controller, dropped while hci0 down */
static pthread_t th;

static int write_all(int fd, const unsigned char *b, size_t n) {
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* Length of the complete H4 packet at b (n bytes available), 0 if incomplete, -1 if the type is unknown. */
static long h4_len(const unsigned char *b, size_t n) {
    if (!n) return 0;
    switch (b[0]) {
    case 0x04: return n < 3 ? 0 : 3 + (long)b[2];                                   /* event */
    case 0x02: return n < 5 ? 0 : 5 + (long)(b[3] | b[4] << 8);                      /* ACL */
    case 0x03: return n < 4 ? 0 : 4 + (long)b[3];                                    /* SCO */
    case 0x05: return n < 5 ? 0 : 5 + (long)((b[3] | b[4] << 8) & 0x3fff);           /* ISO */
    default: return -1;
    }
}

static void set_address(void);

static void *bridge(void *v) {
    (void)v;
    set_address();
    int h4 = open(H4, O_RDWR | O_CLOEXEC);
    if (h4 < 0) { logmsg("bt: %s: %s", H4, strerror(errno)); running = 0; return NULL; }
    int vh = open(VHCI, O_RDWR | O_CLOEXEC);
    if (vh < 0) { logmsg("bt: %s: %s", VHCI, strerror(errno)); close(h4); running = 0; return NULL; }
    /* create hci0 now (HCI_VENDOR_PKT 0xff, device type 0x00 = primary) instead of waiting for vhci's 1 s timeout */
    unsigned char create[2] = {0xff, 0x00};
    (void)!write(vh, create, 2);
    logmsg("bt: bridging %s <-> %s", H4, VHCI);
    static unsigned char in[4096 + 8];
    size_t have = 0;
    unsigned char buf[4096];
    while (want) {
        struct pollfd p[2] = {{.fd = h4, .events = POLLIN}, {.fd = vh, .events = POLLIN}};
        if (poll(p, 2, 500) <= 0) continue;
        if (p[1].revents & POLLIN) { /* host -> controller: vhci gives whole packets, type byte first */
            ssize_t n = read(vh, buf, sizeof buf);
            if (n <= 0) break;
            if (buf[0] == 0xff) continue; /* vhci's own vendor packet (device created) */
            if (write_all(h4, buf, (size_t)n) < 0) { logmsg("bt: write to chip: %s", strerror(errno)); break; }
            pk_down++;
        }
        if (p[0].revents & POLLIN) { /* controller -> host: a byte stream; cut it into packets for vhci */
            ssize_t n = read(h4, in + have, sizeof in - 8 - have);
            if (n <= 0) { logmsg("bt: chip closed (%s)", n < 0 ? strerror(errno) : "eof"); break; }
            have += (size_t)n;
            for (;;) {
                long l = h4_len(in, have);
                if (l < 0) { memmove(in, in + 1, --have); continue; } /* resync on garbage */
                if (l == 0 || (size_t)l > have) break;
                if (write(vh, in, (size_t)l) == l) pk_up++;
                else if (errno == ENXIO) pk_drop++; /* hci0 not up yet: the kernel drops it (e.g. the chip's hello) */
                else { logmsg("bt: write to vhci: %s", strerror(errno)); want = 0; break; }
                memmove(in, in + l, have - (size_t)l);
                have -= (size_t)l;
            }
            if (have >= sizeof in - 8) have = 0;
        }
        if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP)) break;
    }
    close(vh); /* hci0 goes away */
    close(h4); /* the chip's Bluetooth service stops */
    logmsg("bt: bridge stopped");
    running = 0;
    return NULL;
}

/* Use the phone's own Bluetooth address (efs), as Android does, instead of the chip-derived default. */
static void set_address(void) {
    char a[32] = "";
    unsigned u[6];
    if (read_file("/mnt/vendor/efs/bluetooth/bt_addr", a, sizeof a) < 0 ||
        sscanf(a, "%2x:%2x:%2x:%2x:%2x:%2x", &u[0], &u[1], &u[2], &u[3], &u[4], &u[5]) != 6) return;
    unsigned long long v = 0;
    for (int i = 0; i < 6; i++) v = v << 8 | u[i];
    char s[32];
    snprintf(s, sizeof s, "%llu", v);
    int fd = open("/sys/module/scsc_bt/parameters/bluetooth_address", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

static long restart_until; /* bt forget: off for a moment so the kernel drops the old link keys */

/* Should Bluetooth run now? Enabled, and no computer on the cable while "keep off while wired" is on. */
int bt_wanted(void) {
    if (!store_get_int("bt", 0)) return 0;
    if (!store_get_int("bt.wired_off", 1)) return 1;
    return gadget_wired_known() && !gadget_wired();
}

void bt_forget(void) {
    bthid_disconnect();
    store_unset("bt.keys");
    restart_until = now_ms() + 2500;
}

void bt_tick(void) {
    static int last = -1;
    int w = bt_wanted() && now_ms() >= restart_until;
    if (w != last) { if (last >= 0) logmsg("bt: %s", w ? "on" : "off (not needed now)"); last = w; }
    want = w && access(H4, F_OK) == 0;
    bthid_set(want);
    if (want && !running) {
        running = 1;
        if (pthread_create(&th, NULL, bridge, NULL)) running = 0;
        else pthread_detach(th);
    }
}

/* ---- tiny raw-HCI client for `bt scan` (no BlueZ userspace in the image) ---- */
#define AF_BT 31
#define BTPROTO_HCI 1
#define SOL_HCI 0
#define HCI_FILTER 2
#define HCIDEVUP _IOW('H', 201, int)
struct sockaddr_hci { unsigned short hci_family; unsigned short hci_dev; unsigned short hci_channel; };
struct hci_filter { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };

static int bt_scan(char *out, size_t len) {
    int s = socket(AF_BT, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (s < 0) return snprintf(out, len, "error: no Bluetooth socket (%s)\n", strerror(errno));
    if (ioctl(s, HCIDEVUP, 0) < 0 && errno != EALREADY) { int e = errno; close(s); return snprintf(out, len, "error: hci0 up: %s\n", strerror(e)); }
    struct sockaddr_hci a = {AF_BT, 0, 0};
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { int e = errno; close(s); return snprintf(out, len, "error: bind: %s\n", strerror(e)); }
    struct hci_filter f = {1u << 4 /* events */, {0xffffffff, 0xffffffff}, 0};
    setsockopt(s, SOL_HCI, HCI_FILTER, &f, sizeof f);
    size_t o0 = 0;
    {   /* Read_BD_ADDR (0x1009): proves the command path through the bridge */
        unsigned char rb[] = {0x01, 0x09, 0x10, 0};
        if (write(s, rb, sizeof rb) == sizeof rb) {
            long e2 = now_ms() + 2000;
            unsigned char r[64];
            while (now_ms() < e2) {
                struct pollfd p = {.fd = s, .events = POLLIN};
                if (poll(&p, 1, 300) <= 0) continue;
                ssize_t n = read(s, r, sizeof r);
                if (n >= 13 && r[0] == 0x04 && r[1] == 0x0e && r[4] == 0x09 && r[5] == 0x10) {
                    o0 = (size_t)snprintf(out, len, "controller %02X:%02X:%02X:%02X:%02X:%02X (status 0x%02x)\n",
                                          r[12], r[11], r[10], r[9], r[8], r[7], r[6]);
                    break;
                }
            }
        }
    }
    /* HCI_Inquiry: opcode 0x0401, LAP 0x9e8b33 (GIAC), length 6 (x1.28 s), unlimited responses */
    unsigned char cmd[] = {0x01, 0x01, 0x04, 5, 0x33, 0x8b, 0x9e, 6, 0};
    if (write(s, cmd, sizeof cmd) != sizeof cmd) { int e = errno; close(s); return snprintf(out, len, "error: inquiry: %s\n", strerror(e)); }
    size_t o = o0 + (size_t)snprintf(out + o0, len - o0, "inquiry (8 s):\n");
    int events = 0;
    int found = 0;
    long end = now_ms() + 10000;
    unsigned char b[300];
    while (now_ms() < end && o + 64 < len) {
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = read(s, b, sizeof b);
        if (n < 3 || b[0] != 0x04) continue;
        int ev = b[1];
        events++;
        if (ev == 0x01) { o += (size_t)snprintf(out + o, len - o, "  inquiry complete (status 0x%02x)\n", n > 3 ? b[3] : 0xff); break; }
        /* 0x02 inquiry result, 0x22 with RSSI, 0x2f extended: num_responses, then bdaddr (6, little endian) */
        if ((ev == 0x02 || ev == 0x22 || ev == 0x2f) && n >= 10) {
            const unsigned char *d = b + 4;
            o += (size_t)snprintf(out + o, len - o, "  %02X:%02X:%02X:%02X:%02X:%02X\n", d[5], d[4], d[3], d[2], d[1], d[0]);
            found++;
        }
        if (ev == 0x0f && n >= 4 && b[3] != 0) { o += (size_t)snprintf(out + o, len - o, "  command status error 0x%02x\n", b[3]); break; }
    }
    close(s);
    o += (size_t)snprintf(out + o, len - o, "%d device(s) found, %d events\n", found, events);
    return (int)o;
}

int bt_ctl(char **argv, int argc, char *out, size_t len) {
    if (argc > 1 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        store_set("bt", !strcmp(argv[1], "on") ? "1" : "0");
        bt_tick();
        return snprintf(out, len, "bluetooth %s\n", argv[1]);
    }
    if (argc > 2 && !strcmp(argv[1], "wired-off")) {
        store_set("bt.wired_off", !strcmp(argv[2], "on") ? "1" : "0");
        bt_tick();
        return snprintf(out, len, "wired-off %s\n", argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "forget")) { bt_forget(); return snprintf(out, len, "paired host forgotten\n"); }
    if (argc > 1 && !strcmp(argv[1], "scan")) return bt_scan(out, len);
    if (argc > 1 && !strcmp(argv[1], "type") && argc > 2) {
        if (!bthid_connected()) return snprintf(out, len, "error: no Bluetooth host connected\n");
        for (int i = 2; i < argc; i++) { type_hid_text(argv[i]); if (i + 1 < argc) type_hid_text(" "); }
        return snprintf(out, len, "typed\n");
    }
    if (argc > 1 && !strcmp(argv[1], "connect")) { bthid_connect(); return snprintf(out, len, "calling the bonded host (see: bt log)\n"); }
    if (argc > 1 && !strcmp(argv[1], "disconnect")) { bthid_disconnect(); return snprintf(out, len, "disconnecting\n"); }
    if (argc > 1 && !strcmp(argv[1], "log")) return bthid_log(out, len);
    if (argc > 1 && (!strcmp(argv[1], "key") || !strcmp(argv[1], "mouse")) && argc > 2) { /* raw reports, for testing */
        if (!bthid_connected()) return snprintf(out, len, "error: no Bluetooth host connected\n");
        if (argv[1][0] == 'k') { int m = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 0; bthid_key(m, (int)strtol(argv[2], NULL, 0)); usleep(30000); bthid_key(0, 0); }
        else bthid_mouse(0, atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0, 0, 0);
        return snprintf(out, len, "sent\n");
    }
    if (argc > 1 && !strcmp(argv[1], "pair")) { bthid_pair(); return snprintf(out, len, "discoverable as \"PhoneKit\" for 3 minutes: pair it from the other device\n"); }
    char addr[40] = "";
    if (read_file("/sys/class/bluetooth/hci0/address", addr, sizeof addr) < 0 || !addr[0])
        read_file("/mnt/vendor/efs/bluetooth/bt_addr", addr, sizeof addr); /* what set_address() gave the chip */
    addr[strcspn(addr, "\r\n")] = 0;
    return snprintf(out, len, "enabled %d\nwired_off %d\nwired %d\nbridge %s\nhci0 %s\naddress %s\npackets_up %lu\npackets_down %lu\ndropped_while_down %lu\nhid %s\n",
                    store_get_int("bt", 0), store_get_int("bt.wired_off", 1), gadget_wired(), running ? "running" : "stopped",
                    access("/sys/class/bluetooth/hci0", F_OK) == 0 ? "present" : "absent", addr[0] ? addr : "-", pk_up, pk_down, pk_drop, bthid_status());
}
