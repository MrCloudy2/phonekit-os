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
//
// Samsung composite kernels (CONFIG_USB_CONFIGFS_UEVENT + ANDROID_SAMSUNG_COMPOSITE, e.g. Galaxy S8 /
// Exynos 8895; hw.c usb = HW_USB_SAMSUNG_COMPOSITE): a configfs link only parks
// the function; it joins the config when its name is written to android0/functions, and android0/enable
// binds and connects. Writing UDC alone binds an empty config (-EINVAL, "needs at least one function").
// After enable=0 the functions are parked again and can't be unlinked (kernel WARN, function leaks), so
// on these kernels functions are unlinked while still bound, then the gadget is disabled.
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

#define SAMSUNG_USB "/sys/class/android_usb/android0"
static char samsung_fns[256];      /* function names in link order, for android0/functions */
static const char *acm_fns[GADGET_PORTS]; /* the ACM instances that exist (log, ctl, shell) */

/* Declared per phone (hw.c), not guessed: the XCover Pro also has android0, but drives the gadget through UDC. */
static int is_samsung_composite(void) { return phone.usb == HW_USB_SAMSUNG_COMPOSITE; }

/* configfs function dir -> the name Samsung's make_usb_func_link matches: "acm.gs0" -> "acm",
   "rndis.usb0" -> "rndis". Samsung's f_fs.c names EVERY FunctionFS function "adb" (ffs.adb and ffs.gud alike),
   so any "ffs.*" maps to "adb"; paneld links only the FunctionFS functions the mode needs, so that's exact.
   Duplicates are dropped. */
static void samsung_fn_add(const char *fn) {
    char name[64];
    const char *dot = strchr(fn, '.');
    if (!dot) snprintf(name, sizeof name, "%s", fn);
    else if (!strncmp(fn, "ffs.", 4)) snprintf(name, sizeof name, "adb");
    else snprintf(name, sizeof name, "%.*s", (int)(dot - fn), fn);
    size_t nl = strlen(name);
    for (const char *p = samsung_fns; (p = strstr(p, name)); p += nl)
        if ((p == samsung_fns || p[-1] == ',') && (p[nl] == ',' || !p[nl])) return;
    size_t l = strlen(samsung_fns);
    snprintf(samsung_fns + l, sizeof samsung_fns - l, "%s%s", l ? "," : "", name);
}

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
    char pb[256];
    DIR *d = opendir(plan_path("/sys/class/udc", pb, sizeof pb));
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
        if (pl_mkdir(p) == 0 || errno == EEXIST) return names[i];
    }
    return NULL;
}

static int link_function(const char *fn) {
    char src[128], dst[128];
    snprintf(src, sizeof src, G "/functions/%s", fn);
    snprintf(dst, sizeof dst, CFG "/%s", fn);
    if (pl_symlink(src, dst) < 0 && errno != EEXIST) {
        logmsg("usb: link %s: %s", fn, strerror(errno));
        return -1;
    }
    samsung_fn_add(fn);
    return 0;
}

static void udc_unbind(void) {
    if (is_samsung_composite()) { write_file(SAMSUNG_USB "/enable", "0"); return; }
    wr("UDC", "\n");
}

static unsigned bind_gen;
unsigned gadget_bind_gen(void) { return bind_gen; }

static int udc_bind(void) {
    /* Only Samsung's composite layer recreates /dev/hidgN on a rebind. Elsewhere (XCover Pro) the HID fds stay valid
       and are kept open, as before the S8 port: closing an f_hid node and reopening it right after a rebind locked
       up the XCover Pro's 4.14 kernel during the MOUSE tests (2026-11-19). */
    if (!is_samsung_composite()) return wr("UDC", udc);
    bind_gen++;
    /* the same sequence as Samsung's own init (TWRP init.recovery.usb.rc): enable 0, functions, UDC, enable 1 */
    write_file(SAMSUNG_USB "/enable", "0");
    if (write_file(SAMSUNG_USB "/functions", samsung_fns) < 0) {
        logmsg("usb: write android0/functions=%s: %s", samsung_fns, strerror(errno));
        return -1;
    }
    wr("UDC", udc); /* fails harmlessly if enable binds on its own */
    if (write_file(SAMSUNG_USB "/enable", "1") < 0) {
        logmsg("usb: write android0/enable=1: %s", strerror(errno));
        return -1;
    }
    logmsg("usb: Samsung composite: functions %s, enabled", samsung_fns);
    return 0;
}

static void unlink_function(const char *fn) {
    char dst[128];
    snprintf(dst, sizeof dst, CFG "/%s", fn);
    pl_unlink(dst);
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
    char pb[256];
    plan_log("write %s = (%zu-byte report descriptor)", p, n);
    int fd = open(plan_path(p, pb, sizeof pb), O_WRONLY | O_CLOEXEC | (plan_mode ? O_CREAT : 0), 0644);
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
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xE7, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xE7, 0x81, 0x00, /* keys 0..0xE7 (F13-F24) */
    0xC0,
};

/* Consumer control (media keys): one 16-bit usage, no report id. 2-byte report. /dev/hidg3. */
static const uint8_t cons_desc[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};

static const char *mouse_fn, *kbd_fn, *cons_fn;

static int setup_hid(void) {
    uint8_t desc[512];
    size_t n = 0;
    memcpy(desc + n, hid_head, sizeof hid_head); n += sizeof hid_head;
    for (int i = 0; i < HID_CONTACTS; i++) { memcpy(desc + n, hid_finger, sizeof hid_finger); n += sizeof hid_finger; }
    memcpy(desc + n, hid_tail, sizeof hid_tail); n += sizeof hid_tail;
    /* creation order = /dev/hidgN order: 0 touchscreen, 1 mouse, 2 keyboard, 3 media keys */
    const char *t[] = {"hid.usb0", "hid.gs0"}, *m[] = {"hid.usb1", "hid.gs1"}, *k[] = {"hid.usb2", "hid.gs2"},
               *c[] = {"hid.usb3", "hid.gs3"};
    int rc = make_hid(t, 2, &hid_fn, 0, 0, HID_REPORT_LEN, desc, n);
    make_hid(m, 2, &mouse_fn, 0, 0, 7, mouse_desc, sizeof mouse_desc);
    make_hid(k, 2, &kbd_fn, 1, 1, 8, kbd_desc, sizeof kbd_desc);
    make_hid(c, 2, &cons_fn, 0, 0, 2, cons_desc, sizeof cons_desc);
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

void run_quiet(char *const argv[]) {
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
    if (plan_mode) { plan_log("net %s 10.42.0.2/24 up, DHCP server for the host", ifname); return; }
    if (access("/bin/busybox", X_OK) != 0) { logmsg("usb: no busybox: %s left unconfigured", ifname); return; }
    char *up[] = {"/bin/busybox", "ifconfig", ifname, "10.42.0.2", "netmask", "255.255.255.0", "up", NULL};
    run_quiet(up);
    /* DHCP for the host: one lease, no router/DNS (so the host keeps its own default route) unless tethering
     * is on (M10b): then the phone is the host's router and DNS (its Wi-Fi uplink, NATed in wifi.c) */
    char conf[512];
    int tether = store_get_int("tether", 0);
    char dns[64] = "";
    if (tether) { char r[256] = ""; read_file("/run/resolv.conf", r, sizeof r); char *d = strstr(r, "nameserver ");
                  if (d) { sscanf(d + 11, "%63s", dns); } }
    snprintf(conf, sizeof conf,
             "start 10.42.0.1\nend 10.42.0.1\ninterface %s\nmax_leases 1\n"
             "lease_file /run/udhcpd.leases\nopt subnet 255.255.255.0\n%s%s%s%s",
             ifname, tether ? "opt router 10.42.0.2\nopt lease 600\n" : "",
             tether ? "opt dns " : "", tether ? (dns[0] ? dns : "1.1.1.1") : "", tether ? "\n" : "");
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
    if (pl_access("/config/usb_gadget", F_OK) < 0) {
        logmsg("usb: no configfs usb_gadget (is /config mounted?)");
        return -1;
    }
    for (int i = 0; first_udc(udc, sizeof udc) < 0; i++) { /* the UDC can appear late */
        if (i >= 50) { logmsg("usb: no UDC"); return -1; }
        usleep(100000);
    }
    serial_from_cmdline(serial, sizeof serial);

    pl_mkdir(G);
    udc_unbind();
    wr("idVendor", "0x1d6b");  /* Linux Foundation */
    wr("idProduct", "0x0104"); /* Multifunction Composite Gadget */
    wr("bcdDevice", "0x0100");
    wr("bcdUSB", "0x0200");
    wr("bDeviceClass", "0xEF"); /* misc / IAD, so hosts bind per function */
    wr("bDeviceSubClass", "0x02");
    wr("bDeviceProtocol", "0x01");
    pl_mkdir(G "/strings/0x409");
    wr("strings/0x409/manufacturer", "phonepanel");
    wr("strings/0x409/product", "phonepanel");
    wr("strings/0x409/serialnumber", serial);
    pl_mkdir(CFG);
    pl_mkdir(CFG "/strings/0x409");
    wr("configs/c.1/strings/0x409/configuration", phone.usb_config_name);
    wr("configs/c.1/MaxPower", "500");

    const char *names[GADGET_PORTS][3] = {{"acm.gs0", "acm.gs5", "acm.usb0"},
                                          {"acm.gs1", "acm.gs6", "acm.usb1"},
                                          {"acm.gs2", "acm.gs7", "acm.usb2"}};
    const char **fns = acm_fns;
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
    if (adbd_init(G) < 0) logmsg("usb: adb unavailable");

    const char *m = store_get("usb_mode", "B");
    if (gadget_set_mode(m[0]) < 0 && udc_bind() < 0) return -1;

    for (int k = 0; k < GADGET_PORTS; k++) {
        if (!fns[k]) continue;
        int port = k;
        char p[128], name[32];
        snprintf(p, sizeof p, G "/functions/%s/port_num", fns[k]);
        if (read_file(p, buf, sizeof buf) > 0) port = atoi(buf);
        snprintf(name, sizeof name, "ttyGS%d", port);
        snprintf(ttys[k], ttylen, "/dev/%s", name);
        for (int i = 0; !plan_mode && i < 20 && access(ttys[k], F_OK) < 0; i++) {
            ensure_node("tty", name, ttys[k]);
            usleep(50000);
        }
    }
    logmsg("usb: gadget up on %s (serial %s): log %s, ctl %s, shell %s, mode %c", udc, serial,
           ttys[GADGET_LOG][0] ? ttys[GADGET_LOG] : "-", ttys[GADGET_CTL],
           ttys[GADGET_SH][0] ? ttys[GADGET_SH] : "-", mode);
    return 0;
}

int gadget_has_hid(void) { return hid_fn != NULL; }

/* Rewrites the host's DHCP offer (tethering on/off); the host picks it up at its next renewal or replug. */
void gadget_dhcp_refresh(void) { if (mode == 'B' && ncm_fn) ncm_net_up(); }

int gadget_set_mode(char m) {
    if (m != 'A' && m != 'B') return -1;
    long t0 = now_ms();
    int samsung = is_samsung_composite();
    if (!samsung) udc_unbind(); /* unbind: host sees a disconnect */
    if (hid_fn) unlink_function(hid_fn);
    if (mouse_fn) unlink_function(mouse_fn);
    if (kbd_fn) unlink_function(kbd_fn);
    if (cons_fn) unlink_function(cons_fn);
    if (ncm_fn) unlink_function(ncm_fn);
    unlink_function("ffs.gud");
    unlink_function("ffs.adb");
    if (samsung) udc_unbind(); /* after the unlinks: see the top of this file */
    samsung_fns[0] = 0;        /* the ACM ports stay linked, and come first */
    for (int k = 0; k < GADGET_PORTS; k++)
        if (acm_fns[k]) samsung_fn_add(acm_fns[k]);
    /* the host's gud driver binds to vendor interfaces on 1d50:614d (the GUD USB ID) */
    /* Mode B (PHONEBOX) carries adb: present the generic "rndis,adb" Android ID (as postmarketOS/PinePhone do)
     * so stock udev rules give the user's adb access without extra setup. */
    int with_adb = m == 'B' && adbd_ready();
    wr("idVendor", m == 'A' && gud_ready() && hm_wants_gud() ? "0x1d50" : with_adb ? "0x18d1" : "0x1d6b");
    wr("idProduct", m == 'A' && gud_ready() && hm_wants_gud() ? "0x614d" : with_adb ? "0x4ee4" : "0x0104");
    if (dhcpd_pid > 0) { kill(dhcpd_pid, SIGTERM); dhcpd_pid = -1; }
    if (m == 'A' && hid_fn) link_function(hid_fn);
    if (m == 'A' && mouse_fn) link_function(mouse_fn);
    if (m == 'A' && kbd_fn) link_function(kbd_fn);
    if (m == 'A' && cons_fn) link_function(cons_fn);
    if (m == 'A' && gud_ready() && hm_wants_gud()) link_function("ffs.gud");
    int net = ncm_fn && (m == 'B' || hm_input_mode() == IN_RESCUE); /* RESCUE: USB network to the server too */
    if (net) link_function(ncm_fn);
    if (with_adb) link_function("ffs.adb");
    if (udc_bind() < 0) return -1;
    mode = m;
    if (net) { if (net_heals >= 3) net_heals = 0; ncm_net_up(); }
    if (m == 'A' && hid_fn) {
        char p[32];
        for (int i = 0; !plan_mode && i < 20 && access("/dev/hidg0", F_OK) < 0; i++) {
            ensure_node("hidg", "hidg0", "/dev/hidg0");
            usleep(50000);
        }
        ensure_node("hidg", "hidg1", "/dev/hidg1");
        ensure_node("hidg", "hidg2", "/dev/hidg2");
        if (cons_fn) ensure_node("hidg", "hidg3", "/dev/hidg3");
        snprintf(p, sizeof p, "%s", access("/dev/hidg0", F_OK) == 0 ? "/dev/hidg0" : "(missing)");
        logmsg("usb: HID digitizer on %s", p);
    }
    logmsg("usb: mode %c (%s) bound in %ld ms", m, m == 'A' ? gud_ready() ? "display: GUD monitor + HID touch" : "display: HID touch" : "smart: USB network",
           now_ms() - t0);
    return 0;
}

char gadget_mode(void) { return mode; }
const char *gadget_net_if(void) { return net_if; }

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

/* "Wired" for the radios' keep-off-while-wired option: a computer has set the USB link up (or is asleep with it).
 * A plain charger never configures us, so it doesn't count. Debounced: on after 3 s, off after 2 s, so the short
 * unbind/bind of a mode switch doesn't flip the radios. Call often (the main loop does, once a second). */
static int wired_known;
int gadget_wired_known(void) { gadget_wired(); return wired_known; }
int gadget_wired(void) {
    static int state, raw = -1;
    static long since;
    const char *s = gadget_state();
    int w = !strncmp(s, "configured", 10) || !strncmp(s, "suspended", 9);
    long now = now_ms();
    if (w != raw) { raw = w; since = now; }
    /* At boot USB detection is slow (the link stays "not attached" ~12 s on the XCover Pro). Decide "not wired"
     * early only if no power comes in on the cable; with power, give a computer until 25 s to set the link up
     * (else it's a charger). */
    int unwired_ms = 2000;
    if (!wired_known) {
        char u[8] = "", ac[8] = "";
        read_file("/sys/class/power_supply/usb/online", u, sizeof u);
        read_file("/sys/class/power_supply/ac/online", ac, sizeof ac);
        int vbus = u[0] == '1' || ac[0] == '1';
        if (vbus && now < 25000) unwired_ms = 1 << 30; /* not before 25 s */
    }
    if ((state != raw || !wired_known) && now - since >= (raw ? 3000 : unwired_ms)) {
        state = raw;
        wired_known = 1;
        logmsg("usb: %s (radios that keep off while wired follow)", state ? "wired to a computer" : "not wired");
    }
    return state;
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

/* PANELD_PRINT_USB_PLAN=1 (CI, scripts/footprint.py): run the real gadget setup and both USB modes against a
   temporary directory with an empty configfs, one UDC and (for HW_USB_SAMSUNG_COMPOSITE) the android0 files, and
   print every filesystem operation. FunctionFS isn't mounted (adb counts as ready, GUD needs a framebuffer and is
   left out), the USB network isn't configured. What a phone's kernel accepts (configfs function names, Samsung's
   composite rules) isn't simulated: this shows what paneld asks for, so a change to it shows up per phone. */
int gadget_plan(void) {
    char root[] = "/tmp/paneld-plan-XXXXXX", p[160];
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    const char *dirs[] = {"/config", "/config/usb_gadget", "/sys", "/sys/class", "/sys/class/udc", "/sys/class/udc/UDC",
                          "/sys/class/android_usb", "/sys/class/android_usb/android0", NULL};
    for (int i = 0; dirs[i]; i++) {
        if (!is_samsung_composite() && strstr(dirs[i], "android_usb")) continue;
        snprintf(p, sizeof p, "%s%s", root, dirs[i]);
        mkdir(p, 0755);
    }
    plan_set_root(root);
    char ttys[GADGET_PORTS][64];
    printf("# boot (stored usb_mode %s)\n", store_get("usb_mode", "B"));
    int rc = gadget_setup(ttys, NULL);
    printf("# switch to mode A\n");
    gadget_set_mode('A');
    printf("# switch to mode B\n");
    gadget_set_mode('B');
    fflush(stdout);
    char *rm[] = {"/bin/rm", "-rf", root, NULL};
    plan_mode = 0;
    if (fork() == 0) { execv(rm[0], rm); _exit(127); }
    wait(NULL);
    return rc < 0 ? 1 : 0;
}
