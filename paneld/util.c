// Logging, reboot, small file helpers.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int kmsg_fd = -2;
static int logport_fd = -1;
static char logport_path[64];
static unsigned long logport_drained;

void log_port_open(const char *tty) {
    if (!tty || !*tty) return;
    snprintf(logport_path, sizeof logport_path, "%s", tty);
    logport_fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (logport_fd < 0) return;
    struct termios t;
    if (tcgetattr(logport_fd, &t) == 0) { cfmakeraw(&t); tcsetattr(logport_fd, TCSANOW, &t); }
    logmsg("log: mirroring paneld log to %s (read-only port)", tty);
}

int log_port_fd(void) { return logport_fd; }

void log_port_reopen(void) {
    if (!logport_path[0]) return;
    if (logport_fd >= 0) close(logport_fd);
    logport_fd = open(logport_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    struct termios t;
    if (logport_fd >= 0 && tcgetattr(logport_fd, &t) == 0) { cfmakeraw(&t); tcsetattr(logport_fd, TCSANOW, &t); }
}

void log_port_drain(void) {
    char buf[4096];
    ssize_t n;
    while ((n = read(logport_fd, buf, sizeof buf)) > 0) logport_drained += n;
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) log_port_reopen(); /* EOF: hung up */
}

void logmsg(const char *fmt, ...) {
    if (kmsg_fd == -2) kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    char buf[512];
    int n = snprintf(buf, sizeof buf, "<6>paneld: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf - 1) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (kmsg_fd >= 0) (void)!write(kmsg_fd, buf, n);
    if (getpid() != 1) (void)!write(2, buf + 3, n - 3); /* PID 1: kmsg only */
    if (logport_fd >= 0) { /* best effort, never blocks; dropped if nobody reads */
        char line[540];
        int m = snprintf(line, sizeof line, "[%7ld.%03ld] %.*s\r\n", now_ms() / 1000, now_ms() % 1000,
                         n - 4, buf + 3);
        if (m > (int)sizeof line - 1) m = sizeof line - 1;
        (void)!write(logport_fd, line, m);
    }
}

void do_poweroff(const char *why) {
    logmsg("power off: %s", why);
    sync();
    if (getpid() != 1) { logmsg("(not PID 1, not powering off)"); return; }
    reboot(RB_POWER_OFF);
    for (;;) pause();
}

void do_reboot(const char *why, const char *target) {
    logmsg("rebooting%s%s: %s", target ? " to " : "", target ? target : "", why);
    /* remember why, so the next boot can say (kernel logs don't survive a trip through the ROM) */
    char r[96];
    snprintf(r, sizeof r, "%s%s%s @%ld", why, target ? " -> " : "", target ? target : "", (long)time(NULL));
    if (strncmp(why, "watchdog", 8)) store_set("last_reboot", r); /* the watchdog mustn't risk blocking on storage */
    sync();
    if (getpid() != 1) { logmsg("(not PID 1, not rebooting)"); return; }
    if (target && *target)
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, target);
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

/* Is `code` held down right now on any input device? (safe mode at boot) */
int boot_key_held(int code) {
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
        int held = ioctl(fd, EVIOCGKEY(sizeof bits), bits) >= 0 && (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long))) & 1);
        close(fd);
        if (held) return 1;
    }
    return 0;
}

/* If devtmpfs did not give us a node, create it from /sys/class/<cls>/<name>/dev. */
void ensure_node(const char *cls, const char *name, const char *path) {
    if (plan_mode || access(path, F_OK) == 0) return;
    char p[256], buf[32];
    snprintf(p, sizeof p, "/sys/class/%s/%s/dev", cls, name);
    if (read_file(p, buf, sizeof buf) < 0) return;
    unsigned maj, min;
    if (sscanf(buf, "%u:%u", &maj, &min) == 2) {
        char dir[256];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) { *slash = 0; mkdir(dir, 0755); }
        mknod(path, S_IFCHR | 0600, makedev(maj, min));
    }
}

long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* ---- USB plan mode (PANELD_PRINT_USB_PLAN, CI footprints): the gadget code runs against a temporary directory
   instead of /config and /sys, and every filesystem operation it makes is printed, in order, on stdout. */
int plan_mode;
static char plan_root[96];
void plan_set_root(const char *root) { snprintf(plan_root, sizeof plan_root, "%s", root); plan_mode = 1; }
const char *plan_path(const char *p, char *buf, size_t len) {
    if (!plan_mode || p[0] != '/') return p;
    snprintf(buf, len, "%s%s", plan_root, p);
    return buf;
}
void plan_log(const char *fmt, ...) {
    if (!plan_mode) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
int pl_mkdir(const char *p) {
    char b[256];
    if (!plan_mode) return mkdir(p, 0755);
    plan_log("mkdir %s", p);
    plan_path(p, b, sizeof b);
    /* configfs makes a gadget's functions/, configs/ and strings/ itself: create missing parents silently */
    for (char *s = b + strlen(plan_root) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(b, 0755); *s = '/'; }
    return mkdir(b, 0755);
}
int pl_symlink(const char *src, const char *dst) {
    char a[256], b[256];
    plan_log("link %s -> %s", dst, src);
    return symlink(plan_path(src, a, sizeof a), plan_path(dst, b, sizeof b));
}
int pl_unlink(const char *p) { char b[256]; int r = unlink(plan_path(p, b, sizeof b)); if (!r) plan_log("unlink %s", p); return r; }
int pl_access(const char *p, int m) { char b[256]; return access(plan_path(p, b, sizeof b), m); }

int write_file(const char *path, const char *s) {
    char pb[256];
    if (plan_mode) { /* newlines shown as \n, so the plan stays one line per operation */
        char v[160]; size_t o = 0;
        for (const char *c = s; *c && o < sizeof v - 3; c++) { if (*c == '\n') { v[o++] = '\\'; v[o++] = 'n'; } else v[o++] = *c; }
        v[o] = 0;
        plan_log("write %s = %s", path, v);
    }
    path = plan_path(path, pb, sizeof pb);
    int fd = open(path, O_WRONLY | O_CLOEXEC | (plan_mode ? O_CREAT | O_TRUNC : 0), 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = errno;
    close(fd);
    errno = e;
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

int read_file(const char *path, char *buf, size_t len) {
    char pb[256];
    path = plan_path(path, pb, sizeof pb);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return (int)n;
}
