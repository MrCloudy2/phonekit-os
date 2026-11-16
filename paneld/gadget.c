// USB gadget via configfs, with runtime-switchable modes.
//
// Always present, in this interface order (host tools match on interface numbers):
//   iface 0/1 ACM "log"   read-only paneld log (also the decoy for software that grabs ttyACM0)
//   iface 2/3 ACM "ctl"   control channel
//   iface 4/5 ACM "shell" busybox shell
// Mode-specific functions follow:
//   Mode A "display": HID multitouch digitizer (+ GUD display in M6)
//   Mode B "smart":   NCM network (phone 10.42.0.2/24, DHCP for the host)
// Switching = unbind UDC, relink the mode functions, bind again. The ACM instances
// stay, so their ttyGS ports survive (open fds get a hangup and are reopened).
#include "paneld.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define G "/config/usb_gadget/pp"
#define CFG G "/configs/c.1"

static char udc[64];
static char mode = 0; /* 'A' or 'B' once set up */
static const char *hid_fn, *ncm_fn; /* ncm_fn: the mode-B network function (RNDIS or NCM) */

static int wr(const char *rel, const char *val) {
    char p[256];
    snprintf(p, sizeof p, G "/%s", rel);
    if (write_file(p, val) < 0) {
        logmsg("usb: write %s=%s: %s", rel, val, strerror(errno));
        return -1;
    }
    return 0;
}

static void serial_from_cmdline(char *out, size_t len) {
    char cmd[4096];
    snprintf(out, len, "phonepanel");
    if (read_file("/proc/cmdline", cmd, sizeof cmd) < 0) return;
    char *p = strstr(cmd, "androidboot.serialno=");
    if (!p) return;
    p += strlen("androidboot.serialno=");
    size_t n = strcspn(p, " ");
    if (n && n < len) { memcpy(out, p, n); out[n] = 0; }
}

static int first_udc(char *out, size_t len) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return -1;
    struct dirent *e;
    int ok = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(out, len, "%.63s", e->d_name);
        ok = 0;
        break;
    }
    closedir(d);
    return ok;
}

/* mkdir the first instance name the kernel accepts; returns it or NULL */
static const char *make_function(const char *const *names, int n) {
    for (int i = 0; i < n; i++) {
        char p[128];
        snprintf(p, sizeof p, G "/functions/%s", names[i]);
        if (mkdir(p, 0755) == 0 || errno == EEXIST) return names[i];
    }
    return NULL;
}

static int link_function(const char *fn) {
    char src[128], dst[128];
    snprintf(src, sizeof src, G "/functions/%s", fn);
    snprintf(dst, sizeof dst, CFG "/%s", fn);
    if (symlink(src, dst) < 0 && errno != EEXIST) {
        logmsg("usb: link %s: %s", fn, strerror(errno));
        return -1;
    }
    return 0;
}

static void unlink_function(const char *fn) {
    char dst[128];
    snprintf(dst, sizeof dst, CFG "/%s", fn);
    unlink(dst);
}

/* ---------------------------------------------------------------- HID digitizer */

#define HID_CONTACTS 10
/* Multitouch touchscreen, HID_CONTACTS contacts per report (Linux hid-multitouch /
 * Windows-style). Per contact: tip (1 bit) + 7 pad, contact id (8), X (16), Y (16),
 * logical range 0..32767 over the panel. Then contact count (8). Report ID 1. */
static const uint8_t hid_finger[] = {
    0x09, 0x22,       /*   Usage (Finger) */
    0xA1, 0x02,       /*   Collection (Logical) */
    0x09, 0x42,       /*     Usage (Tip Switch) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x75, 0x01,       /*     Report Size (1) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */
    0x75, 0x07,       /*     Report Size (7) */
    0x81, 0x03,       /*     Input (Const) */
    0x09, 0x51,       /*     Usage (Contact Identifier) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767) */
    0x75, 0x10,       /*     Report Size (16) */
    0x09, 0x30,       /*     Usage (X) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */
    0x09, 0x31,       /*     Usage (Y) */
    0x81, 0x02,       /*     Input (Data,Var,Abs) */
    0x05, 0x0D,       /*     Usage Page (Digitizer) */
    0xC0,             /*   End Collection */
};
static const uint8_t hid_head[] = {
    0x05, 0x0D, /* Usage Page (Digitizer) */
    0x09, 0x04, /* Usage (Touch Screen) */
    0xA1, 0x01, /* Collection (Application) */
    0x85, 0x01, /*   Report ID (1) */
};
static const uint8_t hid_tail[] = {
    0x09, 0x54, /*   Usage (Contact Count) */
    0x25, 0x7F, /*   Logical Maximum (127) */
    0x75, 0x08, /*   Report Size (8) */
    0x95, 0x01, /*   Report Count (1) */
    0x81, 0x02, /*   Input (Data,Var,Abs) */
    0x09, 0x55, /*   Usage (Contact Count Maximum) */
    0x25, HID_CONTACTS,
    0xB1, 0x02, /*   Feature (Data,Var,Abs) */
    0xC0,       /* End Collection */
};
#define HID_REPORT_LEN (1 + HID_CONTACTS * 6 + 1)

static int make_hid(const char *const *names, int nn, const char **fn_out, int subclass, int protocol,
                    int report_len, const uint8_t *desc, size_t n) {
    const char *fn = make_function(names, nn);
    if (!fn) { logmsg("usb: no hid function (CONFIG_USB_CONFIGFS_F_HID?)"); return -1; }
    char rel[128], v[16];
    snprintf(rel, sizeof rel, "functions/%s/protocol", fn); snprintf(v, sizeof v, "%d", protocol); wr(rel, v);
    snprintf(rel, sizeof rel, "functions/%s/subclass", fn); snprintf(v, sizeof v, "%d", subclass); wr(rel, v);
    snprintf(rel, sizeof rel, "functions/%s/report_length", fn); snprintf(v, sizeof v, "%d", report_len); wr(rel, v);
    char p[160];
    snprintf(p, sizeof p, G "/functions/%s/report_desc", fn);
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, desc, n) != (ssize_t)n) {
        logmsg("usb: hid report_desc %s: %s", fn, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    *fn_out = fn;
    return 0;
}

/* Relative mouse: 3 buttons, 16-bit X/Y, wheel, AC Pan (horizontal scroll). 7-byte report. */
static const uint8_t mouse_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,             /* Generic Desktop / Mouse / Application */
    0x09, 0x01, 0xA1, 0x00,                         /*   Pointer / Physical */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,             /*     Buttons 1..3 */
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,             /*     padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,             /*     X, Y */
    0x16, 0x01, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, /* -32767..32767, relative */
    0x09, 0x38,                                     /*     Wheel */
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02,                   /*     Consumer / AC Pan */
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0,
};

/* Standard boot keyboard (8-byte report: modifiers, reserved, 6 keys) + LED output. */
static const uint8_t kbd_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

static const char *mouse_fn, *kbd_fn;

static int setup_hid(void) {
    uint8_t desc[512];
    size_t n = 0;
    memcpy(desc + n, hid_head, sizeof hid_head); n += sizeof hid_head;
    for (int i = 0; i < HID_CONTACTS; i++) { memcpy(desc + n, hid_finger, sizeof hid_finger); n += sizeof hid_finger; }
    memcpy(desc + n, hid_tail, sizeof hid_tail); n += sizeof hid_tail;
    /* creation order = /dev/hidgN order: 0 touchscreen, 1 mouse, 2 keyboard */
    const char *t[] = {"hid.usb0", "hid.gs0"}, *m[] = {"hid.usb1", "hid.gs1"}, *k[] = {"hid.usb2", "hid.gs2"};
    int rc = make_hid(t, 2, &hid_fn, 0, 0, HID_REPORT_LEN, desc, n);
    make_hid(m, 2, &mouse_fn, 0, 0, 7, mouse_desc, sizeof mouse_desc);
    make_hid(k, 2, &kbd_fn, 1, 1, 8, kbd_desc, sizeof kbd_desc);
    return rc;
}

/* Builds a report from up to HID_CONTACTS contacts (x/y in 0..32767). */
int gadget_hid_report(uint8_t *buf, const int *ids, const int *xs, const int *ys, const int *down, int n) {
    memset(buf, 0, HID_REPORT_LEN);
    buf[0] = 1;
    int cnt = 0;
    for (int i = 0; i < n && i < HID_CONTACTS; i++) {
        uint8_t *c = buf + 1 + i * 6;
        c[0] = down[i] ? 1 : 0;
        c[1] = (uint8_t)ids[i];
        c[2] = xs[i] & 0xff; c[3] = xs[i] >> 8;
        c[4] = ys[i] & 0xff; c[5] = ys[i] >> 8;
        cnt++;
    }
    buf[1 + HID_CONTACTS * 6] = (uint8_t)cnt;
    return HID_REPORT_LEN;
}

/* ---------------------------------------------------------------- NCM network */

static void run_quiet(char *const argv[]) {
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execv(argv[0], argv);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static pid_t dhcpd_pid = -1;

static char net_if[32] = "usb0";
static long net_bound_ms;
static int net_heals;

static void ncm_net_up(void) {
    char ifname[32] = "usb0", p[128];
    snprintf(p, sizeof p, G "/functions/%s/ifname", ncm_fn);
    read_file(p, ifname, sizeof ifname);
    snprintf(net_if, sizeof net_if, "%s", ifname);
    net_bound_ms = now_ms();
    if (access("/bin/busybox", X_OK) != 0) { logmsg("usb: no busybox: %s left unconfigured", ifname); return; }
    char *up[] = {"/bin/busybox", "ifconfig", ifname, "10.42.0.2", "netmask", "255.255.255.0", "up", NULL};
    run_quiet(up);
    /* DHCP for the host: one lease, no router/DNS (so the host keeps its own default route) */
    char conf[256];
    snprintf(conf, sizeof conf,
             "start 10.42.0.1\nend 10.42.0.1\ninterface %s\nmax_leases 1\n"
             "lease_file /run/udhcpd.leases\nopt subnet 255.255.255.0\n", ifname);
    /* regular files here (write_file() is for sysfs/configfs and doesn't create) */
    int fd = open("/run/udhcpd.conf", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) { (void)!write(fd, conf, strlen(conf)); close(fd); }
    fd = open("/run/udhcpd.leases", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644); /* fresh: MAC changes per function */
    if (fd >= 0) close(fd);
    if (dhcpd_pid > 0) { kill(dhcpd_pid, SIGTERM); dhcpd_pid = -1; }
    pid_t c = fork();
    if (c == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl("/bin/busybox", "udhcpd", "-f", "/run/udhcpd.conf", (char *)NULL);
        _exit(127);
    }
    dhcpd_pid = c;
    logmsg("usb: %s 10.42.0.2/24 up, DHCP offers 10.42.0.1 to the host", ifname);
}

/* Mode B network function. Default RNDIS: on the XCover Pro kernel, Samsung's modified
 * NCM receives but never transmits (TX 0 packets). RNDIS works with Linux (rndis_host)
 * and Windows out of the box; set usb_net=ncm for devices/hosts that prefer NCM (macOS). */
static int setup_ncm(void) {
    const char *rndis[] = {"rndis.usb0", "rndis.gs4"};
    const char *ncm[] = {"ncm.usb0", "ncm.gs9", "ncm.gs0"};
    int want_ncm = !strcmp(store_get("usb_net", "rndis"), "ncm");
    ncm_fn = want_ncm ? make_function(ncm, 3) : make_function(rndis, 2);
    if (!ncm_fn) ncm_fn = want_ncm ? make_function(rndis, 2) : make_function(ncm, 3);
    if (!ncm_fn) { logmsg("usb: no network function"); return -1; }
    logmsg("usb: mode B network function %s", ncm_fn);
    return 0;
}

/* ---------------------------------------------------------------- setup / modes */

int gadget_setup(char ttys[GADGET_PORTS][64], struct fb *f) {
    const size_t ttylen = 64;
    char serial[64], buf[64];
    if (access("/config/usb_gadget", F_OK) < 0) {
        logmsg("usb: no configfs usb_gadget (is /config mounted?)");
        return -1;
    }
    for (int i = 0; first_udc(udc, sizeof udc) < 0; i++) { /* the UDC can appear late */
        if (i >= 50) { logmsg("usb: no UDC"); return -1; }
        usleep(100000);
    }
    serial_from_cmdline(serial, sizeof serial);

    mkdir(G, 0755);
    wr("UDC", "\n");
    wr("idVendor", "0x1d6b");  /* Linux Foundation */
    wr("idProduct", "0x0104"); /* Multifunction Composite Gadget */
    wr("bcdDevice", "0x0100");
    wr("bcdUSB", "0x0200");
    wr("bDeviceClass", "0xEF"); /* misc / IAD, so hosts bind per function */
    wr("bDeviceSubClass", "0x02");
    wr("bDeviceProtocol", "0x01");
    mkdir(G "/strings/0x409", 0755);
    wr("strings/0x409/manufacturer", "phonepanel");
    wr("strings/0x409/product", "phonepanel");
    wr("strings/0x409/serialnumber", serial);
    mkdir(CFG, 0755);
    mkdir(CFG "/strings/0x409", 0755);
    wr("configs/c.1/strings/0x409/configuration", "phonepanel");
    wr("configs/c.1/MaxPower", "500");

    const char *names[GADGET_PORTS][3] = {{"acm.gs0", "acm.gs5", "acm.usb0"},
                                          {"acm.gs1", "acm.gs6", "acm.usb1"},
                                          {"acm.gs2", "acm.gs7", "acm.usb2"}};
    const char *fns[GADGET_PORTS] = {NULL};
    for (int k = 0; k < GADGET_PORTS; k++) {
        ttys[k][0] = 0;
        fns[k] = make_function(names[k], 3);
        if (!fns[k] || link_function(fns[k]) < 0) {
            logmsg("usb: acm port #%d unavailable", k);
            if (k == GADGET_CTL) return -1;
            fns[k] = NULL;
        }
    }
    setup_hid();
    setup_ncm();
    if (f && gud_init(f, G) < 0) logmsg("usb: GUD display unavailable");

    const char *m = store_get("usb_mode", "B");
    if (gadget_set_mode(m[0]) < 0 && wr("UDC", udc) < 0) return -1;

    for (int k = 0; k < GADGET_PORTS; k++) {
        if (!fns[k]) continue;
        int port = k;
        char p[128], name[32];
        snprintf(p, sizeof p, G "/functions/%s/port_num", fns[k]);
        if (read_file(p, buf, sizeof buf) > 0) port = atoi(buf);
        snprintf(name, sizeof name, "ttyGS%d", port);
        snprintf(ttys[k], ttylen, "/dev/%s", name);
        for (int i = 0; i < 20 && access(ttys[k], F_OK) < 0; i++) {
            ensure_node("tty", name, ttys[k]);
            usleep(50000);
        }
    }
    logmsg("usb: gadget up on %s (serial %s): log %s, ctl %s, shell %s, mode %c", udc, serial,
           ttys[GADGET_LOG][0] ? ttys[GADGET_LOG] : "-", ttys[GADGET_CTL],
           ttys[GADGET_SH][0] ? ttys[GADGET_SH] : "-", mode);
    return 0;
}

int gadget_set_mode(char m) {
    if (m != 'A' && m != 'B') return -1;
    long t0 = now_ms();
    wr("UDC", "\n"); /* unbind: host sees a disconnect */
    if (hid_fn) unlink_function(hid_fn);
    if (mouse_fn) unlink_function(mouse_fn);
    if (kbd_fn) unlink_function(kbd_fn);
    if (ncm_fn) unlink_function(ncm_fn);
    unlink_function("ffs.gud");
    /* the host's gud driver binds to vendor interfaces on 1d50:614d (the GUD USB ID) */
    wr("idVendor", m == 'A' && gud_ready() ? "0x1d50" : "0x1d6b");
    wr("idProduct", m == 'A' && gud_ready() ? "0x614d" : "0x0104");
    if (dhcpd_pid > 0) { kill(dhcpd_pid, SIGTERM); dhcpd_pid = -1; }
    if (m == 'A' && hid_fn) link_function(hid_fn);
    if (m == 'A' && mouse_fn) link_function(mouse_fn);
    if (m == 'A' && kbd_fn) link_function(kbd_fn);
    if (m == 'A' && gud_ready()) link_function("ffs.gud");
    if (m == 'B' && ncm_fn) link_function(ncm_fn);
    if (wr("UDC", udc) < 0) return -1;
    mode = m;
    if (m == 'B' && ncm_fn) { if (net_heals >= 3) net_heals = 0; ncm_net_up(); }
    if (m == 'A' && hid_fn) {
        char p[32];
        for (int i = 0; i < 20 && access("/dev/hidg0", F_OK) < 0; i++) {
            ensure_node("hidg", "hidg0", "/dev/hidg0");
            usleep(50000);
        }
        ensure_node("hidg", "hidg1", "/dev/hidg1");
        ensure_node("hidg", "hidg2", "/dev/hidg2");
        snprintf(p, sizeof p, "%s", access("/dev/hidg0", F_OK) == 0 ? "/dev/hidg0" : "(missing)");
        logmsg("usb: HID digitizer on %s", p);
    }
    logmsg("usb: mode %c (%s) bound in %ld ms", m, m == 'A' ? gud_ready() ? "display: GUD monitor + HID touch" : "display: HID touch" : "smart: USB network",
           now_ms() - t0);
    return 0;
}

char gadget_mode(void) { return mode; }

static long net_stat(const char *what) {
    char p[96], b[32];
    snprintf(p, sizeof p, "/sys/class/net/%s/statistics/%s", net_if, what);
    return read_file(p, b, sizeof b) > 0 ? atol(b) : -1;
}

/* Samsung's RNDIS sometimes never transmits after the gadget comes up at boot: the host's
 * frames arrive (RX grows) but nothing goes out (TX 0, host ARP fails). Re-linking the
 * function fixes it (a runtime A->B switch always worked). Checked once a second in mode B. */
void gadget_tick(void) {
    static long last;
    if (mode != 'B' || !ncm_fn || now_ms() - last < 1000) return;
    last = now_ms();
    if (net_heals >= 3 || now_ms() - net_bound_ms < 6000 || strcmp(gadget_state(), "configured")) return;
    long rx = net_stat("rx_packets"), tx = net_stat("tx_packets");
    if (rx >= 8 && tx == 0) {
        net_heals++;
        logmsg("usb: %s RX %ld but TX 0: re-linking the network function (heal #%d)", net_if, rx, net_heals);
        gadget_set_mode('B');
    } else if (tx > 0) net_heals = 3; /* working: stop checking until the next bind */
}

/* UDC state: "configured" when a host has set us up, "not attached", "default", ... */
const char *gadget_state(void) {
    static char st[32];
    char p[128];
    snprintf(p, sizeof p, "/sys/class/udc/%s/state", udc);
    if (read_file(p, st, sizeof st) <= 0) snprintf(st, sizeof st, "unknown");
    return st;
}

/* Simulated unplug/replug (soft disconnect on the D+ pull-up). */
int gadget_reconnect(int off_ms) {
    char p[128];
    snprintf(p, sizeof p, "/sys/class/udc/%s/soft_connect", udc);
    if (write_file(p, "disconnect") < 0) return -1;
    usleep(off_ms * 1000);
    return write_file(p, "connect");
}
