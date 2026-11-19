// Control channel over the USB CDC-ACM tty. Line protocol, see docs/ctl-protocol.md.
//
// request:   <command> [args…]\n          (args split on spaces, "double quotes" group)
// responses: .data <n>\n<n raw bytes>\n   (zero or more)
//            .exit <code>\n               (run only)
//            .ok [text]\n | .err <msg>\n  (always last)
// put <path> <len> [mode] is followed by exactly <len> raw bytes from the host.
#include "paneld.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define LINE_MAX_ 4096
#define MAX_ARGS 64
#define GET_MAX (64u << 20)
#define RUN_MAX (4u << 20)

static int fd = -1;
static char line[LINE_MAX_];
static size_t line_len;

/* put state */
static int put_fd = -1;
static size_t put_left;
static long put_last_ms; /* last payload byte: a put that stalls is abandoned (see ctl_handle) */
static char put_path[512], put_tmp[520];
static mode_t put_mode;

int ctl_fd(void) { return fd; }

static char tty_path[64];

int ctl_open(const char *tty) {
    snprintf(tty_path, sizeof tty_path, "%s", tty);
    fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { logmsg("ctl: open %s: %s", tty, strerror(errno)); return -1; }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(fd, TCSANOW, &t);
    }
    logmsg("ctl: listening on %s", tty);
    return 0;
}

/* After a USB disconnect/mode switch the tty is hung up: reopen it to get a fresh one. */
void ctl_reopen(void) {
    if (!tty_path[0]) return;
    if (fd >= 0) close(fd);
    fd = -1;
    line_len = 0;
    int keep = put_fd; /* abort a half-finished upload */
    if (keep >= 0) { close(keep); put_fd = -1; unlink(put_tmp); put_path[0] = 0; }
    struct termios t;
    fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0 && tcgetattr(fd, &t) == 0) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
}

/* Set when the host stopped reading; replies are dropped until the next valid
 * command arrives, so a deaf host can't stall the main loop 5 s per reply. */
static int out_dead;

/* Write everything or give up after 5 s without progress (host not reading). */
static int out(const void *buf, size_t len) {
    const char *p = buf;
    if (out_dead) return -1;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= n; continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        if (poll(&pf, 1, 5000) <= 0) {
            logmsg("ctl: host is not reading; dropping replies until the next command");
            tcflush(fd, TCOFLUSH);
            out_dead = 1;
            return -1;
        }
    }
    return 0;
}

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    out(buf, n);
}

static void data(const void *buf, size_t len) {
    outf(".data %zu\n", len);
    out(buf, len);
    out("\n", 1);
}

static void ok(const char *msg) { outf(".ok%s%s\n", msg ? " " : "", msg ? msg : ""); }
static void err(const char *msg) { outf(".err %s\n", msg); }
static void err_errno(const char *what) { outf(".err %s: %s\n", what, strerror(errno)); }

static int split(char *s, char **argv) {
    int n = 0;
    while (*s && n < MAX_ARGS - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    argv[n] = NULL;
    return n;
}

/* --------------------------------------------------------------- commands */

static void cmd_info(struct app *a) {
    char buf[4096], v[128];
    int n = 0;
    n += snprintf(buf + n, sizeof buf - n, "version %s\n", PP_VERSION);
    n += snprintf(buf + n, sizeof buf - n, "device %s\n", phone.name); /* devices/<name>, from hw.c */
    n += snprintf(buf + n, sizeof buf - n, "uptime_ms %ld\n", now_ms());
    n += snprintf(buf + n, sizeof buf - n, "paneld_start_ms %ld\n", a->boot_ms);
    n += snprintf(buf + n, sizeof buf - n, "idle_reboot_s %d\n", a->idle_reboot_s);
    if (a->have_fb)
        n += snprintf(buf + n, sizeof buf - n, "fb %dx%d bpp %d stride %d virt %ux%u\nfb_rgb_offsets %u %u %u\nrotation %d logical %dx%d\n",
                      a->fb->pw, a->fb->ph, a->fb->bpp, a->fb->stride,
                      a->fb->var.xres_virtual, a->fb->var.yres_virtual,
                      a->fb->var.red.offset, a->fb->var.green.offset, a->fb->var.blue.offset,
                      a->fb->rot, a->fb->w, a->fb->h);
    for (int i = 0; i < a->nin; i++)
        n += snprintf(buf + n, sizeof buf - n, "input %s %s%s%s\n", a->in[i].node, a->in[i].name,
                      a->in[i].is_touch ? " touch" : "", a->in[i].has_power ? " power" : "");
    char kp[96];
    keys_present(kp, sizeof kp);
    n += snprintf(buf + n, sizeof buf - n, "keys %s\n", kp);
    const char *ps[] = {"capacity", "status", "temp", "voltage_now", "current_now"};
    for (unsigned i = 0; i < sizeof ps / sizeof *ps; i++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/class/power_supply/battery/%s", ps[i]);
        if (read_file(p, v, sizeof v) >= 0)
            n += snprintf(buf + n, sizeof buf - n, "battery_%s %s\n", ps[i], v);
    }
    data(buf, n);
    ok(NULL);
}

static void cmd_ls(const char *path) {
    DIR *d = opendir(path);
    if (!d) { err_errno(path); return; }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    struct dirent *e;
    while (buf && (e = readdir(d))) {
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (lstat(p, &st) < 0) continue;
        char t = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISCHR(st.st_mode) ? 'c'
                 : S_ISBLK(st.st_mode) ? 'b' : '-';
        if (n + 1200 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) break; }
        n += snprintf(buf + n, cap - n, "%c %04o %10lld %s", t, st.st_mode & 07777,
                      (long long)st.st_size, e->d_name);
        if (S_ISLNK(st.st_mode)) {
            char tgt[512];
            ssize_t l = readlink(p, tgt, sizeof tgt - 1);
            if (l > 0) { tgt[l] = 0; n += snprintf(buf + n, cap - n, " -> %s", tgt); }
        }
        buf[n++] = '\n';
    }
    closedir(d);
    if (!buf) { err("out of memory"); return; }
    data(buf, n);
    free(buf);
    ok(NULL);
}

/* Reads until EOF (works for /proc and /sys, whose st_size is 0). */
static char *slurp(int rfd, size_t max, size_t *len) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            if (cap >= max) break;
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        ssize_t r = read(rfd, buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += r;
    }
    *len = n;
    return buf;
}

static void cmd_get(const char *path) {
    /* O_NONBLOCK: never wedge on things like /proc/kmsg or a fifo */
    int rfd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (rfd < 0) { err_errno(path); return; }
    size_t n;
    char *buf = slurp(rfd, GET_MAX, &n);
    close(rfd);
    if (!buf) { err("out of memory"); return; }
    data(buf, n);
    free(buf);
    ok(NULL);
}

static void cmd_put(char **argv, int argc) {
    if (argc < 3) { err("usage: put <path> <len> [mode]"); return; }
    snprintf(put_path, sizeof put_path, "%s", argv[1]);
    snprintf(put_tmp, sizeof put_tmp, "%s.part", put_path);
    put_left = strtoull(argv[2], NULL, 10);
    put_last_ms = now_ms();
    put_mode = argc > 3 ? (mode_t)strtoul(argv[3], NULL, 8) : 0644;
    put_fd = open(put_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, put_mode);
    if (put_fd < 0) {
        /* still have to swallow the bytes; put_fd stays -1 and we discard */
        logmsg("ctl: put %s: %s", put_tmp, strerror(errno));
    }
    if (put_left == 0) { /* finish immediately */
        if (put_fd >= 0) { fchmod(put_fd, put_mode); close(put_fd); put_fd = -1; rename(put_tmp, put_path); ok(NULL); }
        else err_errno(put_tmp);
        put_path[0] = 0;
    }
}

static void put_finish(void) {
    if (put_fd < 0) { err("put failed (see dmesg)"); put_path[0] = 0; return; }
    fchmod(put_fd, put_mode);
    fsync(put_fd);
    close(put_fd);
    put_fd = -1;
    if (rename(put_tmp, put_path) < 0) err_errno("rename");
    else ok(NULL);
    put_path[0] = 0;
}

static void cmd_run(char **argv, int timeout_s) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) { err_errno("pipe"); return; }
    pid_t pid = fork();
    if (pid < 0) { err_errno("fork"); close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        char *envp[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin:/tmp", "HOME=/tmp", "TERM=dumb", NULL};
        execve(argv[0], argv, envp);
        /* try PATH lookup */
        char p[512];
        const char *dirs[] = {"/bin/", "/sbin/", "/tmp/"};
        for (unsigned i = 0; i < 3 && !strchr(argv[0], '/'); i++) {
            snprintf(p, sizeof p, "%s%s", dirs[i], argv[0]);
            execve(p, argv, envp);
        }
        dprintf(1, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    if (!buf) cap = 0;
    long deadline = now_ms() + timeout_s * 1000L;
    int killed = 0;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0 && !killed) { kill(-pid, SIGKILL); kill(pid, SIGKILL); killed = 1; }
        struct pollfd pf = {.fd = pfd[0], .events = POLLIN};
        int r = poll(&pf, 1, killed ? 1000 : (int)(left > 1000 ? 1000 : left));
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { if (killed) break; continue; }
        if (n == cap && cap < RUN_MAX) {
            char *nb = realloc(buf, cap * 2);
            if (nb) { buf = nb; cap *= 2; }
        }
        ssize_t rd;
        if (n < cap) {
            rd = read(pfd[0], buf + n, cap - n);
            if (rd > 0) n += rd;
        } else { /* over RUN_MAX: keep draining, drop the rest */
            char sink[4096];
            rd = read(pfd[0], sink, sizeof sink);
        }
        if (rd < 0 && errno == EINTR) continue;
        if (rd <= 0) break;
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (buf) { data(buf, n); free(buf); }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    outf(".exit %d\n", code);
    ok(killed ? "timeout" : NULL);
}

static void cmd_dmesg(void) {
    int len = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    if (len <= 0) len = 1 << 20;
    char *buf = malloc(len);
    if (!buf) { err("out of memory"); return; }
    int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, len);
    if (n < 0) { err_errno("klogctl"); free(buf); return; }
    data(buf, n);
    free(buf);
    ok(NULL);
}

/* touch tap <px> <py> | ltap <lx> <ly> | swipe <x0> <y0> <x1> <y1> [steps] | multi <k>
 * Synthetic touches through a uinput device (full kernel -> evdev -> paneld path).
 * Panel pixel coordinates, except ltap (logical, i.e. after rotation). Injection
 * runs in a child so the main loop keeps processing (and timing) the events. */
static void cmd_touch(struct app *a, char **argv, int argc) {
    if (argc < 2) { err("usage: touch tap|ltap|swipe|multi …"); return; }
    wd_phase("touch:uinput");
    if (inject_open() < 0) { err_errno("uinput"); return; }
    wd_phase("touch:scan");
    /* make sure the main loop has the virtual device before events flow */
    for (int i = 0; i < 20; i++) {
        int found = 0;
        for (int k = 0; k < a->nin; k++) found |= !strcmp(a->in[k].name, "phonepanel-inject");
        if (found) break;
        usleep(50000);
        a->nin = inputs_scan(a->in, a->nin, MAX_INPUTS);
    }
    const char *m = argv[1];
    int v[5] = {0};
    for (int i = 0; i < 5 && i + 2 < argc; i++) v[i] = atoi(argv[i + 2]);
    if (!strcmp(m, "ltap")) { logical_to_panel(v[0], v[1], &v[0], &v[1]); m = "tap"; }
    wd_phase("touch:fork");
    pid_t p = fork();
    if (p < 0) { err_errno("fork"); return; }
    if (p == 0) {
        setsid(); signal(SIGHUP, SIG_IGN); /* outlive the host closing the ctl tty mid-gesture */
        int rc = 0;
        if (!strcmp(m, "tap")) {
            int xs[3] = {v[0], v[0], v[0]}, ys[3] = {v[1], v[1], v[1]};
            rc = inject_stroke(0, xs, ys, 3, 8);
        } else if (!strcmp(m, "swipe")) {
            int steps = v[4] > 1 ? v[4] : 30, xs[256], ys[256];
            if (steps > 256) steps = 256;
            for (int i = 0; i < steps; i++) {
                xs[i] = v[0] + (v[2] - v[0]) * i / (steps - 1);
                ys[i] = v[1] + (v[3] - v[1]) * i / (steps - 1);
            }
            rc = inject_stroke(0, xs, ys, steps, 8);
        } else if (!strcmp(m, "hold")) { /* hold x y ms: a stationary press */
            int n = v[2] / 10 > 0 ? v[2] / 10 : 1, xs[300], ys[300];
            if (n > 300) n = 300;
            for (int i = 0; i < n; i++) { xs[i] = v[0]; ys[i] = v[1]; }
            rc = inject_stroke(0, xs, ys, n, 10);
        } else if (!strcmp(m, "mtap")) { /* mtap k [y]: k fingers down and up without moving */
            int y = v[1] > 0 ? v[1] : a->fb->ph / 2;
            rc = inject_multi(v[0] > 0 ? v[0] : 2, y, y, 3);
        } else if (!strcmp(m, "mdrag")) { /* mdrag k y0 y1: k fingers dragging vertically */
            rc = inject_multi(v[0] > 0 ? v[0] : 2, v[1], v[2], 25);
        } else if (!strcmp(m, "multi")) {
            rc = inject_multi(v[0] > 0 ? v[0] : 5, a->fb->ph * 3 / 4, a->fb->ph * 3 / 4 + 200, 25);
        } else rc = -2;
        _exit(rc ? 1 : 0);
    }
    wd_phase("touch:reply");
    ok(NULL);
    wd_phase("touch:done");
}

/* Only lines that start with a lowercase command word are commands. Anything
 * else (echoed replies, modem probes, NULs, line noise) is dropped without a
 * reply, so nothing on the host side can start a ping-pong with us. */
static int is_command(const char *l, size_t len) {
    if (!len || l[0] < 'a' || l[0] > 'z') return 0;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)l[i] < 0x20 && l[i] != '\t') return 0;
    return 1;
}

static void log_junk(const char *l, size_t len) {
    static unsigned junk_count;
    if (++junk_count > 5) {
        if (junk_count % 1000 == 0) logmsg("ctl: %u junk lines ignored so far", junk_count);
        return;
    }
    char hex[3 * 24 + 1] = "";
    for (size_t i = 0; i < len && i < 24; i++) snprintf(hex + 3 * i, 4, "%02x ", (unsigned char)l[i]);
    logmsg("ctl: ignoring non-command line (%zu bytes): %s", len, hex);
}

static char l_raw[LINE_MAX_]; /* the line before split() (for commands taking raw JSON) */

static void dispatch(struct app *a, char *l, size_t len) {
    if (!is_command(l, len)) { if (len) log_junk(l, len); return; }
    memcpy(l_raw, l, len + 1);
    out_dead = 0;
    char *argv[MAX_ARGS];
    int argc = split(l, argv);
    if (!argc) return;
    const char *c = argv[0];

    /* Breadcrumb + watchdog phase (static copy: the watchdog keeps the pointer).
     * Only returning to poll() counts as progress, except for commands that
     * may legitimately take long. */
    static char phase[48];
    snprintf(phase, sizeof phase, "ctl:%.40s", c);
    static const char *const logged[] = {"info", "ls", "get", "put", "run", "runt", "dmesg",
                                         "pattern", "idle", "reboot", "stall", "rotate", "power", "usb", "store", NULL};
    for (int i = 0; logged[i]; i++)
        if (!strcmp(c, logged[i]))
            logmsg("ctl: %s%s%s", c, argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
    if (!strcmp(c, "run")) wd_allow(phase, 30);
    else if (!strcmp(c, "runt") && argc > 1) wd_allow(phase, atoi(argv[1]));
    else if (!strcmp(c, "get") || !strcmp(c, "dmesg")) wd_allow(phase, 60);
    else wd_phase(phase);
    if (!strcmp(c, "ping")) { char m[64]; snprintf(m, sizeof m, "pong %ld", now_ms()); ok(m); }
    else if (!strcmp(c, "info")) cmd_info(a);
    else if (!strcmp(c, "ls")) cmd_ls(argc > 1 ? argv[1] : "/");
    else if (!strcmp(c, "get") && argc > 1) cmd_get(argv[1]);
    else if (!strcmp(c, "put")) cmd_put(argv, argc);
    else if (!strcmp(c, "run") && argc > 1) cmd_run(argv + 1, 30);
    else if (!strcmp(c, "runt") && argc > 2) cmd_run(argv + 2, atoi(argv[1]));
    else if (!strcmp(c, "dmesg")) cmd_dmesg();
    else if (!strcmp(c, "touch")) cmd_touch(a, argv, argc);
    else if (!strcmp(c, "keys")) {
        /* keys | keys press <action> (run an action as if a key was pressed) */
        if (argc > 2 && !strcmp(argv[1], "press")) { key_action(argv[2]); ok(NULL); return; }
        /* keys inject <code> <hold_ms>: press a virtual hardware key (real evdev path) */
        if (argc > 3 && !strcmp(argv[1], "inject")) {
            int code = (int)strtol(argv[2], NULL, 0), hold = atoi(argv[3]);
            if (inject_key_open() < 0) { err_errno("uinput"); return; }
            for (int i = 0; i < 20; i++) { /* the main loop must have the device first */
                int found = 0;
                for (int k = 0; k < a->nin; k++) found |= !strcmp(a->in[k].name, "phonepanel-inject-keys");
                if (found) break;
                usleep(50000);
                a->nin = inputs_scan(a->in, a->nin, MAX_INPUTS);
            }
            pid_t p = fork();
            if (p == 0) _exit(inject_key(code, hold) ? 1 : 0);
            ok(NULL);
            return;
        }
        char b[1024];
        int n = keys_dump(b, sizeof b);
        data(b, n);
        ok(NULL);
    } else if (!strcmp(c, "colortest")) {
        /* labelled R/G/B blocks: settles the panel's real channel order by eye */
        struct fb *f = a->fb;
        const char *lab[3] = {"RED", "GREEN", "BLUE"};
        uint32_t col[3] = {fb_color(f, 255, 0, 0), fb_color(f, 0, 255, 0), fb_color(f, 0, 0, 255)};
        int h3 = f->h / 3;
        for (int i = 0; i < 3; i++) {
            fb_fill(f, 0, i * h3, f->w, h3, col[i]);
            fb_text(f, 80, i * h3 + h3 / 2 - 60, 20, lab[i], fb_color(f, 255, 255, 255), col[i]);
        }
        fb_present(f);
        ok(NULL);
    } else if (!strcmp(c, "gud")) {
        char b[256];
        int n = gud_stats(b, sizeof b);
        if (argc > 1 && !strcmp(argv[1], "reset")) gud_stats_reset();
        if (argc > 2 && !strcmp(argv[1], "workers")) gud_set_workers(atoi(argv[2]));
        data(b, n);
        ok(NULL);
    } else if (!strcmp(c, "mode")) {
        /* mode | mode <0..5 or screen mouse keyboard combo rescue deck> | mode phonebox | mode next | mode menu */
        static const char *const names[IN_MODES] = {"screen", "mouse", "keyboard", "combo", "rescue", "deck"};
        int named = -1;
        for (int i = 0; argc > 1 && i < IN_MODES; i++) if (!strcmp(argv[1], names[i])) named = i;
        if (named >= 0) hm_set_mode(named);
        else if (argc > 1 && !strcmp(argv[1], "phonebox")) { ok(NULL); tcdrain(fd); usleep(100000); hm_set_phonebox(); return; }
        else if (argc > 1 && !strcmp(argv[1], "modem")) { ok(NULL); tcdrain(fd); usleep(100000); hm_set_modem(); return; }
        else if (argc > 1 && !strcmp(argv[1], "next")) hm_mode_step(1);
        else if (argc > 1 && !strcmp(argv[1], "menu")) hm_menu_toggle();
        else if (argc > 1 && !strcmp(argv[1], "settings")) hm_open_settings(argc > 2 && !strcmp(argv[2], "wifi") ? NET_WIFI : argc > 2 && !strcmp(argv[2], "bt") ? NET_BT : NET_NONE);
        else if (argc > 1) hm_set_mode(atoi(argv[1]));
        char b2[256];
        int n2 = hm_status(b2, sizeof b2);
        data(b2, n2);
        ok(NULL);
    } else if (!strcmp(c, "panel")) {
        /* panel <json line>: feed PPJ/1 directly (the rest of the line, unsplit) */
        const char *js = strchr(l_raw, ' ');
        static char rep[1024];
        int n2 = js ? panel_handle(js + 1, (int)strlen(js + 1), rep, sizeof rep) : 0;
        data(rep, n2 > 0 ? (size_t)n2 : 0);
        ok(NULL);
    } else if (!strcmp(c, "phonebox") && argc > 1 && !strcmp(argv[1], "hits")) {
        static char b2[4096];
        data(b2, (size_t)pb_hits(b2, sizeof b2));
        ok(NULL);
    } else if (!strcmp(c, "phonebox") && argc > 2 && !strcmp(argv[1], "mute")) {
        pb_agent_muted = atoi(argv[2]) != 0;
        logmsg("phonebox: agent %s", pb_agent_muted ? "muted (test mode)" : "unmuted");
        ok(NULL);
    } else if (!strcmp(c, "phonebox") && argc > 2 && !strcmp(argv[1], "page")) {
        if (pb_page_set(atoi(argv[2])) < 0) err("no such phonebox page"); else ok(NULL);
    } else if (!strcmp(c, "deck")) {
        static char b2[4096];
        int n2 = deck_ctl(argv, argc, l_raw, b2, sizeof b2);
        data(b2, (size_t)n2);
        ok(NULL);
    } else if (!strcmp(c, "bt")) {
        static char b2[2048];
        if (argc > 1 && !strcmp(argv[1], "scan")) wd_allow("ctl:bt scan", 20);
        int n2 = bt_ctl(argv, argc, b2, sizeof b2);
        data(b2, (size_t)n2);
        ok(NULL);
    } else if (!strcmp(c, "ssh")) {
        char b2[512];
        int n2 = ssh_ctl(argv, argc, l_raw, b2, sizeof b2);
        data(b2, (size_t)n2);
        ok(NULL);
    } else if (!strcmp(c, "wifi")) {
        char b2[512];
        int n2 = wifi_ctl(argv, argc, b2, sizeof b2);
        if (n2 == -2) {
            char *scan[] = {"/bin/sh", "-c", "ifconfig wlan0 up; iw dev wlan0 scan 2>&1 | grep -E 'SSID:|signal:|^BSS'", NULL};
            wd_allow("ctl:wifi scan", 30);
            cmd_run(scan, 25);
            return;
        }
        data(b2, (size_t)n2);
        ok(NULL);
    } else if (!strcmp(c, "rescue")) {
        /* rescue | rescue type <text…> (raw line after "type ", \r appended) */
        if (argc > 1 && !strcmp(argv[1], "type")) {
            const char *t = strstr(l_raw, "type ");
            static char b3[1024];
            const char *x = t ? t + 5 : "";
            size_t xl = strlen(x);
            if (xl >= 2 && x[0] == '"' && x[xl - 1] == '"') { x++; xl -= 2; } /* ppctl keeps the quotes */
            snprintf(b3, sizeof b3, "%.*s\r", (int)xl, x);
            term_type(b3);
        }
        char b2[512];
        int n2 = term_stats(b2, sizeof b2);
        data(b2, n2);
        ok(NULL);
    } else if (!strcmp(c, "adb") || !strcmp(c, "phonebox")) {
        char b2[512];
        int n2 = c[0] == 'a' ? adbd_stats(b2, sizeof b2) : pb_stats(b2, sizeof b2);
        data(b2, n2);
        ok(NULL);
    } else if (!strcmp(c, "sensors")) {
        char b2[256];
        int n2 = sensors_stats(b2, sizeof b2);
        data(b2, n2);
        ok(NULL);
    } else if (!strcmp(c, "page")) {
        if (argc > 1) ui_page_set(atoi(argv[1]));
        char m[8]; snprintf(m, sizeof m, "%d", ui_page()); ok(m);
    } else if (!strcmp(c, "usb")) {
        /* usb | usb mode A|B (persisted, re-enumerates) | usb reconnect [off_ms] */
        if (argc > 2 && !strcmp(argv[1], "mode") && (argv[2][0] == 'A' || argv[2][0] == 'B')) {
            char m[2] = {argv[2][0], 0};
            store_set("usb_mode", m);
            ok(m);           /* reply first: the switch hangs up this tty */
            tcdrain(fd);
            usleep(100000);
            gadget_set_mode(m[0]);
            return;
        }
        if (argc > 1 && !strcmp(argv[1], "reconnect")) {
            ok(NULL);
            tcdrain(fd);
            usleep(100000);
            gadget_reconnect(argc > 2 ? atoi(argv[2]) : 500);
            return;
        }
        char b[96];
        snprintf(b, sizeof b, "mode %c\nstate %s\nhid %s\n", gadget_mode(), gadget_state(), gadget_has_hid() ? "yes" : "no");
        data(b, strlen(b));
        ok(NULL);
    } else if (!strcmp(c, "store")) {
        /* store | store set <key> <value> */
        /* store | store set <key> <value> | store unset <key> */
        if (argc > 3 && !strcmp(argv[1], "set")) {
            if (store_set(argv[2], argv[3]) < 0) { err("store write failed"); return; }
        } else if (argc > 2 && !strcmp(argv[1], "unset")) {
            if (store_unset(argv[2]) < 0) { err("store write failed"); return; }
        }
        static char b[8192];
        int n = store_dump(b, sizeof b);
        data(b, n);
        ok(NULL);
    } else if (!strcmp(c, "power")) {
        char buf[1024];
        int n = power_ctl(argv, argc, buf, sizeof buf);
        if (n < 0) { err("usage: power [wake|off|tick|set <key> <value>]"); return; }
        data(buf, n);
        ok(NULL);
    } else if (!strcmp(c, "fstats")) {
        char buf[256];
        int n = fb_stats(a->fb, buf, sizeof buf);
        if (argc > 1 && !strcmp(argv[1], "reset")) fb_stats_reset(a->fb);
        data(buf, n);
        ok(NULL);
    } else if (!strcmp(c, "clock") && argc > 1) {
        a->clock_on = !strcmp(argv[1], "on") || !strcmp(argv[1], "1");
        ok(a->clock_on ? "on" : "off");
    } else if (!strcmp(c, "blank") && argc > 1) {
        /* blank on: DPMS powerdown (panel + DSI off); blank off: unblank and redraw */
        if (!a->have_fb) { err("no framebuffer"); return; }
        int on = !strcmp(argv[1], "on") || !strcmp(argv[1], "1");
        if (fb_blank(a->fb, on) < 0) { err_errno("FBIOBLANK"); return; }
        ok(on ? "blanked" : "unblanked");
    } else if (!strcmp(c, "fbdump")) {
        /* visible frame from paneld's own mapping (DECON fbdev has no read()) */
        if (!a->have_fb) { err("no framebuffer"); return; }
        size_t off = (size_t)a->fb->var.yoffset * a->fb->stride, len = (size_t)a->fb->ph * a->fb->stride;
        if (off + len > a->fb->len) off = 0;
        wd_allow("ctl:fbdump", 60);
        data(a->fb->mem + off, len);
        ok(NULL);
    }
    else if (!strcmp(c, "rotate")) {
        if (argc > 1) { hm_set_rotation(atoi(argv[1])); }
        char m[16]; snprintf(m, sizeof m, "%d", touch_rotation()); ok(m);
    } else if (!strcmp(c, "tstats")) {
        char buf[512];
        int n = touch_stats(buf, sizeof buf);
        if (argc > 1 && !strcmp(argv[1], "reset")) touch_stats_reset();
        data(buf, n);
        ok(NULL);
    }
    else if (!strcmp(c, "stall") && argc > 1) { /* debug: freeze the main loop to test the watchdog */
        ok(NULL);
        sleep(atoi(argv[1]));
    }
    else if (!strcmp(c, "pattern")) {
        if (a->have_fb) { draw_test_pattern(a->fb); ok(NULL); } else err("no framebuffer");
    } else if (!strcmp(c, "idle")) {
        if (argc > 1) a->idle_reboot_s = atoi(argv[1]);
        char m[32]; snprintf(m, sizeof m, "%d", a->idle_reboot_s); ok(m);
    } else if (!strcmp(c, "reboot")) {
        /* targets: (none)=normal boot, recovery, download, bootloader */
        ok(NULL);
        tcdrain(fd);
        usleep(200000);
        do_reboot("ctl", argc > 1 ? argv[1] : NULL);
    } else if (!strcmp(c, "help")) {
        static const char h[] =
            "ping | info | ls [dir] | get <path> | put <path> <len> [mode] + bytes\n"
            "run <cmd> [args] | runt <secs> <cmd> [args] | dmesg | pattern\n"
            "idle [secs] (0 = never auto-reboot) | reboot [recovery|download|bootloader]\n"
            "touch tap x y | ltap lx ly | swipe x0 y0 x1 y1 [steps] | hold x y ms | multi k\n"
            "rotate [0|90|180|270] | tstats [reset] | stall secs\n";
        data(h, sizeof h - 1);
        ok(NULL);
    } else {
        char m[160]; snprintf(m, sizeof m, "unknown command '%.100s' (try help)", c); err(m);
    }
}

int ctl_handle(struct app *a) {
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) return -1; /* hung up */
    if (n < 0) return 0;
    a->last_activity_ms = now_ms();
    if (put_path[0] && put_left && now_ms() - put_last_ms > 10000) {
        /* the host gave up half-way (crashed client, cable): don't treat its next commands as file data */
        logmsg("ctl: put %s abandoned with %zu bytes missing", put_path, put_left);
        if (put_fd >= 0) { close(put_fd); put_fd = -1; }
        unlink(put_tmp);
        put_path[0] = 0;
        put_left = 0;
    }
    size_t i = 0;
    while (i < (size_t)n) {
        if (put_path[0] && put_left) { /* raw bytes of a put */
            put_last_ms = now_ms();
            size_t take = (size_t)n - i < put_left ? (size_t)n - i : put_left;
            if (put_fd >= 0 && write(put_fd, buf + i, take) != (ssize_t)take) {
                logmsg("ctl: put write: %s", strerror(errno));
                close(put_fd); put_fd = -1; unlink(put_tmp);
            }
            put_left -= take;
            i += take;
            if (!put_left) put_finish();
            continue;
        }
        char ch = buf[i++];
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (line_len < sizeof line - 1) line[line_len++] = ch;
            continue;
        }
        size_t len = line_len;
        line[len] = 0;
        line_len = 0;
        dispatch(a, line, len);
    }
    return 0;
}
