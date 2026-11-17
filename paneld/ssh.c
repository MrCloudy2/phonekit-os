// Remote access (M11b): dropbear SSH, key-only (password auth isn't even compiled in), running only when at least
// one public key is authorised. `ssh root@<phone>` gives PhoneKit's shell; `ssh root@<phone> pk-console` attaches to
// the RESCUE console (the server's serial login) while the phone shows that a remote session has it.
//
//   ppctl ssh                         status
//   ppctl ssh authorize <public key>  add a key (one "ssh-ed25519 AAAA… comment" line)
//   ppctl ssh revoke                  remove all keys (dropbear stops)
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB "/bin/dropbearmulti"
static pid_t db_pid = -1;
static long next_try;

static int write_text(const char *path, const char *s, mode_t mode) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    int ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok ? 0 : -1;
}

/* Host key: generated once, kept (hex) in the settings store so the phone's fingerprint survives reboots/updates. */
static int host_key(void) {
    mkdir("/run/dropbear", 0700);
    const char *hex = store_get("ssh.hostkey", "");
    if (hex[0]) {
        size_t n = strlen(hex) / 2;
        unsigned char *b = malloc(n);
        for (size_t i = 0; i < n; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); b[i] = (unsigned char)v; }
        int fd = open("/run/dropbear/hostkey", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
        int ok = fd >= 0 && write(fd, b, n) == (ssize_t)n;
        if (fd >= 0) close(fd);
        free(b);
        return ok ? 0 : -1;
    }
    unlink("/run/dropbear/hostkey");
    char *gen[] = {DB, "dropbearkey", "-t", "ed25519", "-f", "/run/dropbear/hostkey", NULL};
    run_quiet(gen);
    int fd = open("/run/dropbear/hostkey", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char b[1024];
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n <= 0) return -1;
    char *h = malloc((size_t)n * 2 + 1);
    for (ssize_t i = 0; i < n; i++) sprintf(h + 2 * i, "%02x", b[i]);
    store_set("ssh.hostkey", h);
    free(h);
    logmsg("ssh: generated a host key");
    return 0;
}

static void prepare(void) {
    /* interactive sessions need pseudo-terminals: devpts + /dev/ptmx (nothing else in PhoneKit used them) */
    mkdir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "ptmxmode=0666,mode=0620,gid=0") < 0 && errno != EBUSY)
        logmsg("ssh: mount devpts: %s", strerror(errno));
    if (access("/dev/ptmx", F_OK) != 0) symlink("pts/ptmx", "/dev/ptmx");
    /* dropbear looks the user up in /etc/passwd and reads ~/.ssh/authorized_keys */
    write_text("/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n", 0644);
    write_text("/etc/group", "root:x:0:\n", 0644);
    write_text("/etc/shells", "/bin/sh\n", 0644);
    mkdir("/root", 0700);
    mkdir("/root/.ssh", 0700);
    char keys[4096];
    snprintf(keys, sizeof keys, "%s", store_get("ssh.keys", ""));
    for (char *p = keys; *p; p++) if (*p == '|') *p = '\n';
    strncat(keys, "\n", sizeof keys - strlen(keys) - 1);
    write_text("/root/.ssh/authorized_keys", keys, 0600);
    write_text("/bin/pk-console",
               "#!/bin/sh\n"
               "# attach this SSH session to the RESCUE console (the server's serial login). Leave: Enter ~ .\n"
               "[ -e /dev/ttyGS2 ] || { echo 'no console port'; exit 1; }\n"
               "touch /run/rescue.remote\n"
               "cleanup() { rm -f /run/rescue.remote; kill $RD 2>/dev/null; stty sane 2>/dev/null; }\n"
               "trap cleanup EXIT INT TERM HUP\n"
               "sleep 0.3\n"
               "echo 'PhoneKit: connected to the server console. Leave with: Enter ~ .'\n"
               "stty -F /dev/ttyGS2 raw -echo 2>/dev/null\n"
               "stty raw -echo 2>/dev/null\n"
               "cat /dev/ttyGS2 & RD=$!\n"
               "cat > /dev/ttyGS2\n", 0755);
}

void ssh_tick(void) {
    if (now_ms() < next_try) return;
    next_try = now_ms() + 5000;
    int want = store_get("ssh.keys", "")[0] && access(DB, X_OK) == 0;
    if (db_pid > 0 && waitpid(db_pid, NULL, WNOHANG) == db_pid) db_pid = -1;
    if (!want) {
        if (db_pid > 0) { kill(db_pid, SIGTERM); db_pid = -1; logmsg("ssh: stopped (no keys)"); }
        return;
    }
    if (db_pid > 0) return;
    prepare();
    if (host_key() < 0) { logmsg("ssh: no host key"); return; }
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        setsid();
        setenv("PATH", "/bin:/sbin", 1);
        /* -F foreground, -j/-k no port forwarding. Passwords and syslog aren't compiled in (key-only, stderr). */
        execl(DB, "dropbear", "-F", "-j", "-k", "-r", "/run/dropbear/hostkey", "-P", "/run/dropbear/pid", "-p", "22", (char *)NULL);
        _exit(127);
    }
    db_pid = p;
    logmsg("ssh: dropbear listening on port 22 (key-only)");
}

/* Called from ctl; returns bytes written to out. */
int ssh_ctl(char **argv, int argc, const char *raw, char *out, size_t len) {
    if (argc > 1 && !strcmp(argv[1], "authorize")) {
        const char *k = raw ? strstr(raw, "authorize ") : NULL;
        k = k ? k + 10 : "";
        while (*k == ' ' || *k == '"') k++;
        char key[1024];
        snprintf(key, sizeof key, "%s", k);
        size_t l = strlen(key);
        while (l && (key[l - 1] == '"' || key[l - 1] == ' ' || key[l - 1] == '\n' || key[l - 1] == '\r')) key[--l] = 0;
        if (strncmp(key, "ssh-", 4) && strncmp(key, "ecdsa-", 6)) return snprintf(out, len, "error: not an OpenSSH public key line\n");
        if (strchr(key, '|')) return snprintf(out, len, "error: unexpected '|' in key\n");
        char all[4096];
        const char *cur = store_get("ssh.keys", "");
        snprintf(all, sizeof all, "%s%s%s", cur, cur[0] ? "|" : "", key);
        store_set("ssh.keys", all);
        next_try = 0;
        if (db_pid > 0) { kill(db_pid, SIGTERM); db_pid = -1; } /* restart with the new keys file */
        return snprintf(out, len, "key added\n");
    }
    if (argc > 1 && !strcmp(argv[1], "revoke")) {
        store_set("ssh.keys", "");
        next_try = 0;
        return snprintf(out, len, "all keys removed; ssh stops\n");
    }
    int nkeys = 0;
    for (const char *p = store_get("ssh.keys", ""); *p; p++) nkeys += *p == '|';
    if (store_get("ssh.keys", "")[0]) nkeys++;
    return snprintf(out, len, "running %s\nkeys %d\nport 22\nremote_console %s\n", db_pid > 0 ? "yes" : "no", nkeys,
                    access("/run/rescue.remote", F_OK) == 0 ? "attached" : "free");
}
