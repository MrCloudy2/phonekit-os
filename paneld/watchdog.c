// Software watchdog for paneld's main loop.
//
// The main loop calls wd_beat() every iteration (and before slow steps). A
// separate thread checks the heartbeat; if it stalls past the allowed time it
// logs where the main thread is (phase, kernel stack, wchan, syscall), asks the
// kernel to dump blocked tasks (sysrq w), and reboots to the normal boot
// partition so the log can be read from the ROM's /proc/last_kmsg.
#include "paneld.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define WD_DEFAULT_S 15

static _Atomic long beat_ms;
static _Atomic int allow_s = WD_DEFAULT_S;
static const char *_Atomic phase = "start";
static pid_t main_tid;

void wd_beat(const char *p) {
    phase = p;
    allow_s = WD_DEFAULT_S;
    beat_ms = now_ms();
}

void wd_phase(const char *p) { phase = p; }

void wd_allow(const char *p, int secs) {
    phase = p;
    allow_s = secs + WD_DEFAULT_S;
    beat_ms = now_ms();
}

static void dump_file(const char *label, const char *path) {
    char buf[2048];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { logmsg("wd: %s: (unreadable)", label); return; }
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) { logmsg("wd: %s: (empty)", label); return; }
    buf[n] = 0;
    char *save, *line = strtok_r(buf, "\n", &save);
    while (line) {
        logmsg("wd: %s: %s", label, line);
        line = strtok_r(NULL, "\n", &save);
    }
}

static void *wd_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(2);
        long stalled = now_ms() - beat_ms;
        if (stalled <= allow_s * 1000L) continue;
        char p[96];
        logmsg("wd: main loop stuck for %ld ms in phase '%s' — dumping and rebooting", stalled, phase);
        snprintf(p, sizeof p, "/proc/self/task/%d/wchan", main_tid);
        dump_file("wchan", p);
        snprintf(p, sizeof p, "/proc/self/task/%d/syscall", main_tid);
        dump_file("syscall", p);
        snprintf(p, sizeof p, "/proc/self/task/%d/stack", main_tid);
        dump_file("stack", p);
        write_file("/proc/sysrq-trigger", "w"); /* blocked tasks → kernel log */
        sleep(1);
        do_reboot("watchdog", NULL);
        return NULL; /* not PID 1 (testing): give up watching */
    }
}

void wd_start(void) {
    main_tid = (pid_t)syscall(SYS_gettid);
    beat_ms = now_ms();
    pthread_t t;
    if (pthread_create(&t, NULL, wd_thread, NULL) != 0) logmsg("wd: cannot start thread");
    else pthread_detach(t);
}
