// Interactive busybox shell on the second USB serial port, plus child reaping.
//
// Host: `picocom /dev/ttyACM1` (or `screen /dev/ttyACM1`). The shell is
// respawned when it exits; as PID 1 we also reap every orphan.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define BUSYBOX "/bin/busybox"

static pid_t sh_pid = -1;
static char sh_tty[64];
static long sh_next_ms;

void shell_setup(void) {
    if (access(BUSYBOX, X_OK) != 0) return;
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl(BUSYBOX, "busybox", "--install", "-s", "/bin", (char *)NULL);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
    logmsg("shell: busybox applets installed in /bin");
}

static void spawn(void) {
    pid_t p = fork();
    if (p < 0) { logmsg("shell: fork: %s", strerror(errno)); return; }
    if (p == 0) {
        setsid();
        int fd = open(sh_tty, O_RDWR); /* becomes the controlling tty */
        if (fd < 0) _exit(126);
        ioctl(fd, TIOCSCTTY, 0);
        struct termios t;
        if (tcgetattr(fd, &t) == 0) { /* sane interactive defaults */
            t.c_iflag |= ICRNL;
            t.c_oflag |= OPOST | ONLCR;
            t.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
            tcsetattr(fd, TCSANOW, &t);
        }
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
        signal(SIGPIPE, SIG_DFL);
        char *envp[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/tmp", "TERM=vt100",
                        "PS1=phonepanel:\\w# ", "ENV=", NULL};
        /* Print nothing until the host sends a line: a freshly opened host tty
         * echoes incoming bytes back before the terminal program sets raw mode,
         * which would feed our own prompt back in as a command. */
        execle(BUSYBOX, "sh", "-c", "read -r _; exec /bin/sh -l", (char *)NULL, envp);
        _exit(127);
    }
    sh_pid = p;
}

void shell_start(const char *tty) {
    if (!tty || !*tty || access(BUSYBOX, X_OK) != 0) return;
    snprintf(sh_tty, sizeof sh_tty, "%s", tty);
    spawn();
    logmsg("shell: busybox sh on %s (pid %d)", sh_tty, sh_pid);
}

void shell_reap(void) {
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0)
        if (p == sh_pid) {
            sh_pid = -1;
            sh_next_ms = now_ms() + 1000; /* don't spin if the tty is dead */
        }
    if (sh_tty[0] && sh_pid < 0 && now_ms() >= sh_next_ms) spawn();
}
