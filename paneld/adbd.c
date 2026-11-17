// Minimal adb device daemon over FunctionFS (PHONEBOX mode).
//
// Enough of the adb protocol for the stock phonebox desktop agent to treat PhoneKit as a phone with the
// phonebox app: `adb devices`, `adb shell <cmd>` (Android commands the agent sends are emulated, everything
// else runs in busybox sh), `adb reverse tcp:A tcp:B` (a listener on the phone's 127.0.0.1:A tunnels each
// connection to the PC's port B), `adb root`, `adb reboot [recovery|download|bootloader]`.
//
// No RSA auth (an "insecure" device, like ro.adb.secure=0): the USB control channel already gives a root shell
// to whoever holds the cable. No shell_v2 (exit codes are not reported; the agent doesn't need them).
#include "paneld.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FFS_DIR "/dev/usb-ffs/adb"
#define A_CNXN 0x4e584e43
#define A_AUTH 0x48545541
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_VERSION 0x01000001
#define MAX_PAYLOAD (256 * 1024)
#define MAX_STREAMS 32
#define MAX_REVERSE 4

struct amsg { uint32_t cmd, arg0, arg1, len, crc, magic; };

static int ep0 = -1, ep_out = -1, ep_in = -1;
static volatile int enabled, online;
static uint32_t host_max = 4096;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static int notify_w = -1, notify_r = -1;
static unsigned long n_msgs, n_shells, n_tunnels, n_replugs;
static volatile long last_rx_ms, enabled_ms, tx_since_ms; /* host traffic, ENABLE time, a bulk-IN write in progress */
static volatile int had_host, host_suspended;

/* ---------------------------------------------------------------- streams */

enum { S_FREE, S_SHELL, S_TUNNEL, S_REPLY };
struct stream {
    int kind;
    uint32_t id, peer;        /* our id, host's id */
    int fd;                   /* socket (tunnel) or shell stdin pipe; data from the host goes here */
    int out_fd;               /* where we read data for the host (socket or shell stdout) */
    pid_t pid;
    int okay;                 /* the host acked our last WRTE */
    int closed;
    int discard_input;        /* emulated install: swallow the APK */
    long t_open;              /* tunnel: when we sent OPEN (the host answers OKAY or CLSE at once) */
    pthread_cond_t cv;
};
static struct stream st[MAX_STREAMS];
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_id = 1;

static struct { int used; int port; char remote[32]; char local[32]; int lfd; } rev[MAX_REVERSE];
static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;

static int write_all(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *b, size_t n) {
    char *p = b;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, uint32_t len) {
    struct amsg m = {cmd, a0, a1, len, 0, cmd ^ 0xffffffffu};
    for (uint32_t i = 0; i < len; i++) m.crc += ((const uint8_t *)data)[i];
    pthread_mutex_lock(&wlock);
    tx_since_ms = now_ms(); /* a host whose adb stopped reading leaves this write blocked */
    int r = ep_in < 0 ? -1 : write_all(ep_in, &m, sizeof m);
    if (!r && len) r = write_all(ep_in, data, len);
    tx_since_ms = 0;
    pthread_mutex_unlock(&wlock);
    return r;
}

static struct stream *stream_new(int kind, uint32_t peer) {
    pthread_mutex_lock(&slock);
    struct stream *s = NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (st[i].kind == S_FREE) { s = &st[i]; break; }
    if (s) {
        pthread_cond_t cv = s->cv;
        memset(s, 0, sizeof *s);
        s->cv = cv;
        s->kind = kind; s->id = next_id++; s->peer = peer; s->fd = s->out_fd = -1; s->okay = 1;
    }
    pthread_mutex_unlock(&slock);
    return s;
}

static struct stream *stream_find(uint32_t id) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (st[i].kind != S_FREE && st[i].id == id) return &st[i];
    return NULL;
}

/* Sends data to the host on stream s, honouring flow control (one WRTE in flight). Returns -1 when closed. */
static int stream_write(struct stream *s, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        pthread_mutex_lock(&slock);
        while (!s->okay && !s->closed && online) pthread_cond_wait(&s->cv, &slock);
        int dead = s->closed || !online;
        if (!dead) s->okay = 0;
        uint32_t id = s->id, peer = s->peer;
        pthread_mutex_unlock(&slock);
        if (dead) return -1;
        size_t chunk = n < host_max ? n : host_max;
        if (send_msg(A_WRTE, id, peer, p, (uint32_t)chunk) < 0) return -1;
        p += chunk; n -= chunk;
    }
    return 0;
}

/* Closes our side: tells the host, frees the slot (fds are closed by the owner thread). */
static void stream_close(struct stream *s, int tell_host) {
    pthread_mutex_lock(&slock);
    int was = s->closed;
    s->closed = 1;
    uint32_t id = s->id, peer = s->peer;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&slock);
    if (!was && tell_host && online) send_msg(A_CLSE, id, peer, NULL, 0);
}

static void stream_free(struct stream *s) {
    pthread_mutex_lock(&slock);
    if (s->fd >= 0) close(s->fd);
    if (s->out_fd >= 0 && s->out_fd != s->fd) close(s->out_fd);
    s->fd = s->out_fd = -1;
    s->kind = S_FREE;
    pthread_mutex_unlock(&slock);
}

/* ---------------------------------------------------------------- emulated Android commands */

static void notify(char c) { if (notify_w >= 0) (void)!write(notify_w, &c, 1); }

static const char *prop(const char *k) {
    static const char *const P[][2] = {
        {"ro.phonekit", "1"},
        {"ro.product.manufacturer", "PhoneKit"},
        {"ro.product.model", "PhoneKit"},
        {"ro.product.name", "phonekit"},
        {"ro.build.version.sdk", "34"},
        {"ro.build.version.release", "14"},
        {"ro.secure", "0"},
        {"ro.debuggable", "1"},
        {"service.adb.root", "1"},
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (!strcmp(P[i][0], k)) return P[i][1];
    return "";
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

/* Returns 1 and fills `out` if the command is one we answer ourselves. */
static int emulate(const char *cmd, char *out, size_t len, int *discard) {
    while (*cmd == ' ') cmd++;
    out[0] = 0;
    if (starts(cmd, "getprop")) {
        const char *k = cmd + 7;
        while (*k == ' ') k++;
        if (!*k) snprintf(out, len, "[ro.phonekit]: [1]\n[ro.product.model]: [PhoneKit]\n");
        else snprintf(out, len, "%s\n", prop(k));
        return 1;
    }
    if (starts(cmd, "pm path net.oxog.phonebox.deskpanel")) {
        snprintf(out, len, "package:/system/app/PhoneKit/phonebox-native.apk\n");
        return 1;
    }
    if (starts(cmd, "pm list packages")) { snprintf(out, len, "package:net.oxog.phonebox.deskpanel\n"); return 1; }
    if (starts(cmd, "am start")) {
        notify('o');
        snprintf(out, len, "Starting: Intent { cmp=net.oxog.phonebox.deskpanel/.MainActivity }\n");
        return 1;
    }
    if (starts(cmd, "input keyevent")) {
        const char *k = cmd + 14;
        while (*k == ' ') k++;
        if (!strcmp(k, "KEYCODE_WAKEUP") || !strcmp(k, "224")) notify('w');
        else if (!strcmp(k, "KEYCODE_SLEEP") || !strcmp(k, "223")) notify('s');
        else if (!strcmp(k, "KEYCODE_POWER") || !strcmp(k, "26")) notify('p');
        return 1;
    }
    if (starts(cmd, "cmd package set-home-activity")) { snprintf(out, len, "Success\n"); return 1; }
    if (starts(cmd, "cmd package install") || starts(cmd, "pm install")) {
        *discard = 1;
        snprintf(out, len, "Success\n");
        return 1;
    }
    if (starts(cmd, "settings get")) { snprintf(out, len, "null\n"); return 1; }
    if (starts(cmd, "settings put") || starts(cmd, "wm ") || starts(cmd, "svc ") || starts(cmd, "cmd ") ||
        starts(cmd, "am ") || starts(cmd, "pm ") || starts(cmd, "monkey"))
        return 1;
    return 0;
}

/* ---------------------------------------------------------------- shell streams */

struct shell_arg { struct stream *s; char cmd[1024]; };

static void *shell_thread(void *v) {
    struct shell_arg *a = v;
    struct stream *s = a->s;
    char buf[16384];
    int disc = 0;
    if (a->cmd[0] && emulate(a->cmd, buf, sizeof buf, &disc)) {
        if (disc) {
            /* an install streams the APK on the stream: wait for the host to finish, then answer */
            s->discard_input = 1;
            for (int i = 0; i < 6000 && !s->closed; i++) usleep(10000);
        }
        if (buf[0]) stream_write(s, buf, strlen(buf));
        stream_close(s, 1);
        stream_free(s);
        free(a);
        return NULL;
    }
    int in[2], outp[2];
    if (pipe2(in, O_CLOEXEC) < 0 || pipe2(outp, O_CLOEXEC) < 0) { stream_close(s, 1); stream_free(s); free(a); return NULL; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(outp[1], 1); dup2(outp[1], 2);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        setenv("PATH", "/bin:/sbin", 1);
        setenv("HOME", "/", 1);
        if (a->cmd[0]) execl("/bin/busybox", "sh", "-c", a->cmd, (char *)NULL);
        else execl("/bin/busybox", "sh", "-i", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(outp[1]);
    pthread_mutex_lock(&slock);
    s->fd = in[1]; s->out_fd = outp[0]; s->pid = pid;
    pthread_mutex_unlock(&slock);
    n_shells++;
    for (;;) {
        ssize_t n = read(outp[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (stream_write(s, buf, (size_t)n) < 0) break;
    }
    if (pid > 0) { kill(-pid, SIGHUP); kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    stream_close(s, 1);
    stream_free(s);
    free(a);
    return NULL;
}

/* ---------------------------------------------------------------- reverse tunnels */

static int parse_tcp(const char *spec) { return starts(spec, "tcp:") ? atoi(spec + 4) : -1; }

struct tun_arg { int fd; char remote[32]; };

static void *tunnel_thread(void *v) {
    struct tun_arg *a = v;
    struct stream *s = stream_new(S_TUNNEL, 0);
    if (!s) { close(a->fd); free(a); return NULL; }
    s->fd = s->out_fd = a->fd;
    s->okay = 0; /* wait for the host's OKAY to our OPEN */
    s->t_open = now_ms();
    char dest[40];
    int dl = snprintf(dest, sizeof dest, "%s", a->remote) + 1;
    n_tunnels++;
    if (send_msg(A_OPEN, s->id, 0, dest, (uint32_t)dl) < 0) { stream_close(s, 0); stream_free(s); free(a); return NULL; }
    /* wait until the host connected (OKAY: peer id set) or refused (CLSE) */
    pthread_mutex_lock(&slock);
    while (!s->peer && !s->closed && online) pthread_cond_wait(&s->cv, &slock);
    pthread_mutex_unlock(&slock);
    char buf[16384];
    while (!s->closed) {
        ssize_t n = read(a->fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (stream_write(s, buf, (size_t)n) < 0) break;
    }
    shutdown(a->fd, SHUT_RDWR);
    stream_close(s, 1);
    stream_free(s);
    free(a);
    return NULL;
}

static void *listen_thread(void *v) {
    int slot = (int)(long)v;
    for (;;) {
        pthread_mutex_lock(&rlock);
        int lfd = rev[slot].used ? rev[slot].lfd : -1;
        pthread_mutex_unlock(&rlock);
        if (lfd < 0) return NULL;
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; return NULL; }
        if (!online) { close(c); continue; }
        struct tun_arg *a = calloc(1, sizeof *a);
        a->fd = c;
        pthread_mutex_lock(&rlock);
        snprintf(a->remote, sizeof a->remote, "%s", rev[slot].remote);
        pthread_mutex_unlock(&rlock);
        pthread_t t;
        if (pthread_create(&t, NULL, tunnel_thread, a)) { close(c); free(a); continue; }
        pthread_detach(t);
    }
}

static void rev_kill(int i) {
    if (!rev[i].used) return;
    rev[i].used = 0;
    shutdown(rev[i].lfd, SHUT_RDWR);
    close(rev[i].lfd);
}

/* "forward:[norebind:]LOCAL;REMOTE" | "list-forward" | "killforward:LOCAL" | "killforward-all" */
static void reverse_cmd(const char *c, char *out, size_t len) {
    pthread_mutex_lock(&rlock);
    if (!strcmp(c, "list-forward")) {
        char list[512] = "";
        size_t o = 0;
        for (int i = 0; i < MAX_REVERSE; i++)
            if (rev[i].used) o += (size_t)snprintf(list + o, sizeof list - o, "(reverse) %s %s\n", rev[i].local, rev[i].remote);
        snprintf(out, len, "%04zx%s", strlen(list), list);
    } else if (!strcmp(c, "killforward-all")) {
        for (int i = 0; i < MAX_REVERSE; i++) rev_kill(i);
        snprintf(out, len, "OKAY");
    } else if (starts(c, "killforward:")) {
        int found = 0;
        for (int i = 0; i < MAX_REVERSE; i++)
            if (rev[i].used && !strcmp(rev[i].local, c + 12)) { rev_kill(i); found = 1; }
        if (found) snprintf(out, len, "OKAY");
        else snprintf(out, len, "FAIL%04zx%s", strlen("listener not found"), "listener not found");
    } else if (starts(c, "forward:")) {
        const char *spec = c + 8;
        int norebind = 0;
        if (starts(spec, "norebind:")) { norebind = 1; spec += 9; }
        char local[32] = "", remote[32] = "";
        const char *semi = strchr(spec, ';');
        if (semi) {
            snprintf(local, sizeof local, "%.*s", (int)(semi - spec), spec);
            snprintf(remote, sizeof remote, "%s", semi + 1);
        }
        int port = parse_tcp(local);
        const char *fail = NULL;
        int slot = -1;
        if (port <= 0 || parse_tcp(remote) <= 0) fail = "only tcp:PORT is supported";
        for (int i = 0; !fail && i < MAX_REVERSE; i++)
            if (rev[i].used && rev[i].port == port) {
                if (norebind) fail = "cannot rebind existing socket";
                else { snprintf(rev[i].remote, sizeof rev[i].remote, "%s", remote); slot = -2; }
            }
        if (!fail && slot == -1) {
            for (int i = 0; i < MAX_REVERSE; i++) if (!rev[i].used) { slot = i; break; }
            if (slot < 0) fail = "too many reverse forwards";
        }
        if (!fail && slot >= 0) {
            int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
            struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                     .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
            setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (l < 0 || bind(l, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(l, 8) < 0) {
                if (l >= 0) close(l);
                fail = "cannot bind listener";
            } else {
                rev[slot].used = 1; rev[slot].port = port; rev[slot].lfd = l;
                snprintf(rev[slot].local, sizeof rev[slot].local, "%s", local);
                snprintf(rev[slot].remote, sizeof rev[slot].remote, "%s", remote);
                pthread_t t;
                pthread_create(&t, NULL, listen_thread, (void *)(long)slot);
                pthread_detach(t);
                logmsg("adb: reverse %s -> host %s", local, remote);
            }
        }
        if (fail) snprintf(out, len, "FAIL%04zx%s", strlen(fail), fail);
        else snprintf(out, len, "OKAY");
    } else {
        snprintf(out, len, "FAIL%04zx%s", strlen("unknown reverse command"), "unknown reverse command");
    }
    pthread_mutex_unlock(&rlock);
}

/* ---------------------------------------------------------------- message handling */

static void reply_and_close(uint32_t peer, const char *data) {
    struct stream *s = stream_new(S_REPLY, peer);
    if (!s) { send_msg(A_CLSE, 0, peer, NULL, 0); return; }
    send_msg(A_OKAY, s->id, peer, NULL, 0);
    if (data && *data) stream_write(s, data, strlen(data));
    stream_close(s, 1);
    stream_free(s);
}

static void *reboot_thread(void *v) {
    sleep(1);
    do_reboot("adb reboot", (const char *)v);
    return NULL;
}

static void handle_open(uint32_t peer, char *svc, uint32_t len) {
    if (len == 0) return;
    svc[len - 1] = 0;
    if (starts(svc, "shell:") || starts(svc, "exec:") || starts(svc, "shell,")) {
        const char *cmd = strchr(svc, ':') + 1;
        struct stream *s = stream_new(S_SHELL, peer);
        if (!s) { send_msg(A_CLSE, 0, peer, NULL, 0); return; }
        struct shell_arg *a = calloc(1, sizeof *a);
        a->s = s;
        snprintf(a->cmd, sizeof a->cmd, "%s", cmd);
        send_msg(A_OKAY, s->id, peer, NULL, 0);
        pthread_t t;
        if (pthread_create(&t, NULL, shell_thread, a)) { free(a); stream_close(s, 1); stream_free(s); return; }
        pthread_detach(t);
        return;
    }
    if (starts(svc, "reverse:")) {
        char out[600];
        reverse_cmd(svc + 8, out, sizeof out);
        reply_and_close(peer, out);
        return;
    }
    if (starts(svc, "root:") || starts(svc, "unroot:")) { reply_and_close(peer, "adbd is already running as root\n"); return; }
    if (starts(svc, "reboot:")) {
        static char target[32];
        snprintf(target, sizeof target, "%.31s", svc + 7);
        if (!strcmp(target, "bootloader")) snprintf(target, sizeof target, "download");
        reply_and_close(peer, "");
        pthread_t t;
        pthread_create(&t, NULL, reboot_thread, target[0] ? target : NULL);
        pthread_detach(t);
        return;
    }
    logmsg("adb: unsupported service '%.40s'", svc);
    send_msg(A_CLSE, 0, peer, NULL, 0);
}

static void all_streams_close(void) {
    pthread_mutex_lock(&slock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (st[i].kind != S_FREE) {
            st[i].closed = 1;
            pthread_cond_broadcast(&st[i].cv);
            if (st[i].kind == S_TUNNEL && st[i].fd >= 0) shutdown(st[i].fd, SHUT_RDWR);
            if (st[i].pid > 0) kill(-st[i].pid, SIGHUP);
        }
    pthread_mutex_unlock(&slock);
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < MAX_REVERSE; i++) rev_kill(i); /* the host re-creates them after reconnecting */
    pthread_mutex_unlock(&rlock);
}

static void handle(struct amsg *m, char *data) {
    n_msgs++;
    switch (m->cmd) {
    case A_CNXN: {
        host_max = m->arg1 < MAX_PAYLOAD ? m->arg1 : MAX_PAYLOAD;
        if (host_max < 1024) host_max = 1024;
        if (online) all_streams_close(); /* host adb restarted */
        online = 1;
        had_host = 1;
        char banner[256];
        int n = snprintf(banner, sizeof banner,
                         "device::ro.product.name=phonekit;ro.product.model=PhoneKit;ro.product.device=phonekit;features=");
        send_msg(A_CNXN, A_VERSION, MAX_PAYLOAD, banner, (uint32_t)n);
        logmsg("adb: host connected (max payload %u)", host_max);
        break;
    }
    case A_AUTH: break; /* we never ask for it */
    case A_OPEN: handle_open(m->arg0, data, m->len); break;
    case A_OKAY: {
        pthread_mutex_lock(&slock);
        struct stream *s = stream_find(m->arg1);
        if (s) { if (!s->peer) s->peer = m->arg0; s->okay = 1; pthread_cond_broadcast(&s->cv); }
        pthread_mutex_unlock(&slock);
        break;
    }
    case A_WRTE: {
        pthread_mutex_lock(&slock);
        struct stream *s = stream_find(m->arg1);
        int fd = s && !s->discard_input ? s->fd : -1, disc = s && s->discard_input;
        uint32_t id = s ? s->id : 0;
        pthread_mutex_unlock(&slock);
        if (!s) { send_msg(A_CLSE, 0, m->arg0, NULL, 0); break; }
        if (fd >= 0 && write_all(fd, data, m->len) < 0) { stream_close(s, 1); break; }
        (void)disc;
        send_msg(A_OKAY, id, m->arg0, NULL, 0);
        break;
    }
    case A_CLSE: {
        pthread_mutex_lock(&slock);
        struct stream *s = stream_find(m->arg1);
        if (s) {
            s->closed = 1;
            pthread_cond_broadcast(&s->cv);
            if (s->kind == S_TUNNEL && s->fd >= 0) shutdown(s->fd, SHUT_RDWR);
            if (s->kind == S_SHELL) {
                if (s->fd >= 0) { close(s->fd); s->fd = -1; } /* EOF on the shell's stdin */
                if (s->pid > 0) kill(-s->pid, SIGHUP);
            }
        }
        pthread_mutex_unlock(&slock);
        break;
    }
    default: break;
    }
}

static void *bulk_thread(void *v) {
    (void)v;
    static char data[MAX_PAYLOAD];
    for (;;) {
        if (!enabled || ep_out < 0) { usleep(100000); continue; }
        struct amsg m;
        if (read_all(ep_out, &m, sizeof m) < 0) { usleep(100000); continue; }
        if (m.magic != (m.cmd ^ 0xffffffffu) || m.len > MAX_PAYLOAD) { logmsg("adb: bad header %08x", m.cmd); continue; }
        if (m.len && read_all(ep_out, data, m.len) < 0) continue;
        last_rx_ms = now_ms();
        handle(&m, data);
    }
    return NULL;
}

/* ---------------------------------------------------------------- FunctionFS */

static int write_descriptors(void) {
    struct __attribute__((packed)) {
        struct usb_functionfs_descs_head_v2 header;
        uint32_t fs_count, hs_count;
        struct __attribute__((packed)) {
            struct usb_interface_descriptor intf;
            struct usb_endpoint_descriptor_no_audio out, in;
        } fs, hs;
    } d;
    memset(&d, 0, sizeof d);
    d.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.header.length = htole32(sizeof d);
    d.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.fs_count = d.hs_count = htole32(3);
    struct usb_interface_descriptor intf = {
        .bLength = sizeof intf, .bDescriptorType = USB_DT_INTERFACE, .bNumEndpoints = 2,
        .bInterfaceClass = 0xff, .bInterfaceSubClass = 0x42, .bInterfaceProtocol = 0x01, .iInterface = 1};
    d.fs.intf = d.hs.intf = intf;
    struct usb_endpoint_descriptor_no_audio out = {
        .bLength = sizeof out, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 1 | USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK};
    struct usb_endpoint_descriptor_no_audio in = out;
    in.bEndpointAddress = 2 | USB_DIR_IN;
    d.fs.out = d.hs.out = out;
    d.fs.in = d.hs.in = in;
    d.fs.out.wMaxPacketSize = d.fs.in.wMaxPacketSize = htole16(64);
    d.hs.out.wMaxPacketSize = d.hs.in.wMaxPacketSize = htole16(512);
    struct __attribute__((packed)) {
        struct usb_functionfs_strings_head header;
        struct __attribute__((packed)) { uint16_t code; char str1[sizeof "ADB Interface"]; } lang0;
    } s = {
        .header = {.magic = htole32(FUNCTIONFS_STRINGS_MAGIC), .length = htole32(sizeof s),
                   .str_count = htole32(1), .lang_count = htole32(1)},
        .lang0 = {htole16(0x0409), "ADB Interface"},
    };
    if (write(ep0, &d, sizeof d) != sizeof d) { logmsg("adb: descriptors: %s", strerror(errno)); return -1; }
    if (write(ep0, &s, sizeof s) != sizeof s) { logmsg("adb: strings: %s", strerror(errno)); return -1; }
    return 0;
}

static void *ep0_thread(void *v) {
    (void)v;
    struct usb_functionfs_event ev[4];
    for (;;) {
        struct pollfd pf = {.fd = ep0, .events = POLLIN};
        if (poll(&pf, 1, 1000) <= 0) continue;
        ssize_t n = read(ep0, ev, sizeof ev);
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; sleep(1); continue; }
        for (int i = 0; i < (int)(n / (ssize_t)sizeof ev[0]); i++) {
            switch (ev[i].type) {
            case FUNCTIONFS_ENABLE:
                if (ep_out < 0) ep_out = open(FFS_DIR "/ep1", O_RDONLY | O_CLOEXEC);
                if (ep_in < 0) ep_in = open(FFS_DIR "/ep2", O_WRONLY | O_CLOEXEC);
                enabled = 1;
                host_suspended = 0;
                enabled_ms = now_ms();
                logmsg("adb: function enabled (ep %d/%d)", ep_out, ep_in);
                break;
            case FUNCTIONFS_DISABLE:
            case FUNCTIONFS_UNBIND:
                if (enabled) logmsg("adb: function disabled");
                enabled = 0;
                online = 0;
                all_streams_close();
                break;
            case FUNCTIONFS_SUSPEND: host_suspended = 1; break; /* the PC went to sleep */
            case FUNCTIONFS_RESUME: host_suspended = 0; enabled_ms = now_ms(); break;
            case FUNCTIONFS_SETUP: {
                /* no class requests: stall by reading/writing nothing useful */
                if (ev[i].u.setup.bRequestType & USB_DIR_IN) (void)!write(ep0, NULL, 0);
                else (void)!read(ep0, NULL, 0);
                break;
            }
            default: break;
            }
        }
    }
    return NULL;
}

static int ready;

int adbd_init(const char *configfs_gadget) {
    char p[160];
    for (int i = 0; i < MAX_STREAMS; i++) pthread_cond_init(&st[i].cv, NULL);
    snprintf(p, sizeof p, "%s/functions/ffs.adb", configfs_gadget);
    if (mkdir(p, 0755) < 0 && errno != EEXIST) { logmsg("adb: %s: %s", p, strerror(errno)); return -1; }
    mkdir("/dev/usb-ffs", 0755);
    mkdir(FFS_DIR, 0755);
    if (mount("adb", FFS_DIR, "functionfs", 0, NULL) < 0 && errno != EBUSY) {
        logmsg("adb: mount functionfs: %s", strerror(errno));
        return -1;
    }
    ep0 = open(FFS_DIR "/ep0", O_RDWR | O_CLOEXEC);
    if (ep0 < 0 || write_descriptors() < 0) { logmsg("adb: ep0: %s", strerror(errno)); return -1; }
    int pf[2];
    if (pipe2(pf, O_CLOEXEC | O_NONBLOCK) == 0) { notify_r = pf[0]; notify_w = pf[1]; }
    /* loopback for the reverse listeners (127.0.0.1) */
    char *lo[] = {"/bin/busybox", "ifconfig", "lo", "127.0.0.1", "up", NULL};
    run_quiet(lo);
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_create(&t, NULL, ep0_thread, NULL); pthread_detach(t);
    pthread_create(&t, NULL, bulk_thread, NULL); pthread_detach(t);
    ready = 1;
    logmsg("adb: FunctionFS ready");
    return 0;
}

int adbd_ready(void) { return ready; }

/* After the PC wakes from sleep, its adb server can drop the phone without the USB link going down: it never
 * talks to us again, and only a new hotplug makes it look. Two signs, counted only while the link is up and
 * awake (never while the PC sleeps, so this can't wake it):
 *   - online, but the host leaves a tunnel OPEN or a bulk write unanswered (a live host answers at once);
 *   - the function was (re-)enabled, and the host that used adb before never sends CNXN.
 * Then simulate an unplug/replug. At most 3 tries per outage (10-15 s, 45 s, 2 min); a CNXN resets them, and
 * nothing happens until some host has used adb since boot (a PC without adb is left alone). Once a second. */
void adbd_tick(void) {
    static long last, next_ok;
    static int stall_s, tries;
    long now = now_ms();
    if (now - last < 1000) return;
    last = now;
    if (online && last_rx_ms > enabled_ms && tries) { tries = 0; next_ok = 0; } /* healthy again */
    int awake = enabled && !host_suspended && !strncmp(gadget_state(), "configured", 10);
    if (!ready || !had_host || !awake || gadget_mode() != 'B') { stall_s = 0; return; }
    const char *why = NULL;
    if (online) {
        long oldest = tx_since_ms;
        pthread_mutex_lock(&slock);
        for (int i = 0; i < MAX_STREAMS; i++)
            if (st[i].kind == S_TUNNEL && !st[i].peer && !st[i].closed && st[i].t_open && (!oldest || st[i].t_open < oldest))
                oldest = st[i].t_open;
        pthread_mutex_unlock(&slock);
        stall_s = oldest && last_rx_ms < oldest ? stall_s + 1 : 0;
        if (stall_s >= 10) why = "the PC stopped answering adb";
    } else {
        stall_s = 0;
        if (now - enabled_ms >= 15000) why = "the PC's adb never came back";
    }
    if (!why || tries >= 3 || now < next_ok) return;
    tries++;
    n_replugs++;
    next_ok = now + (tries == 1 ? 45000 : 120000);
    enabled_ms = now; /* the re-enable restarts the wait anyway */
    stall_s = 0;
    logmsg("adb: %s (after the PC slept?): replugging USB (try %d of 3)", why, tries);
    gadget_reconnect(1000); /* blocked writes fail with the disconnect; the host sees a new device */
}

/* 1 if the host set up `adb reverse tcp:<port> …` (the phonebox agent's tunnel) and is connected. */
int adbd_reverse_active(int port) {
    int r = 0;
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < MAX_REVERSE; i++) if (rev[i].used && rev[i].port == port) r = online;
    pthread_mutex_unlock(&rlock);
    return r;
}
int adbd_notify_fd(void) { return notify_r; }

/* Main loop: one pending wake/sleep/open request from the host, or 0. */
int adbd_notify_read(void) {
    char c;
    return notify_r >= 0 && read(notify_r, &c, 1) == 1 ? c : 0;
}

int adbd_stats(char *b, size_t len) {
    int streams = 0, revs = 0;
    for (int i = 0; i < MAX_STREAMS; i++) if (st[i].kind != S_FREE) streams++;
    for (int i = 0; i < MAX_REVERSE; i++) if (rev[i].used) revs++;
    return snprintf(b, len, "ready %d\nenabled %d\nonline %d\nmsgs %lu\nshells %lu\ntunnels %lu\nstreams %d\nreverse %d\nreplugs %lu\nsuspended %d\n",
                    ready, enabled, online, n_msgs, n_shells, n_tunnels, streams, revs, n_replugs, host_suspended);
}
