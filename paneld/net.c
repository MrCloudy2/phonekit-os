// PPJ/1 over TCP (port 7700) on the USB network (PANEL mode). One client at a time;
// a new connection replaces the old one. Non-blocking; the main loop polls our fds.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 7700
#define LINE_MAX_NET 16384

static int lfd = -1, cfd = -1;
static char buf[LINE_MAX_NET];
static int net_reset_discard;
static size_t blen;

static void send_line(const char *s) {
    if (cfd < 0) return;
    size_t n = strlen(s);
    /* best effort, never blocks the main loop: a client that doesn't read loses events */
    if (send(cfd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n ||
        send(cfd, "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT) != 1) {
        if (errno != EAGAIN) { close(cfd); cfd = -1; }
    }
}

int net_init(void) {
    lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) return -1;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 2) < 0) {
        logmsg("net: listen :%d: %s", PORT, strerror(errno));
        close(lfd); lfd = -1;
        return -1;
    }
    panel_set_event_sink(send_line);
    logmsg("net: PPJ/1 on tcp :%d", PORT);
    return 0;
}

int net_fd(int which) { return which ? cfd : lfd; }

void net_handle(int which) {
    if (which == 0) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept4(lfd, (struct sockaddr *)&peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) return;
        /* Until Wi-Fi pairing exists (M11), only the USB link (10.42.0.0/24) and the phone itself may drive the
         * panel: on a joined Wi-Fi network anyone could connect otherwise. */
        uint32_t ip = ntohl(peer.sin_addr.s_addr);
        if ((ip & 0xffffff00) != 0x0a2a0000 && (ip >> 24) != 127) {
            logmsg("net: refused %u.%u.%u.%u (not on the USB link)", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
            close(c);
            return;
        }
        if (cfd >= 0) close(cfd); /* newest client wins */
        cfd = c;
        blen = 0;
        net_reset_discard = 1;
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        logmsg("net: client connected");
        return;
    }
    static int discarding; /* inside an over-long line: skip to its newline */
    ssize_t n = recv(cfd, buf + blen, sizeof buf - blen, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN)) { logmsg("net: client gone"); close(cfd); cfd = -1; return; }
    if (n < 0) return;
    if (net_reset_discard) { discarding = 0; net_reset_discard = 0; }
    blen += n;
    size_t start = 0;
    for (size_t i = 0; i < blen; i++) {
        if (buf[i] != '\n') continue;
        if (discarding) { discarding = 0; start = i + 1; continue; }
        static char reply[1024];
        if (i > start && panel_handle(buf + start, (int)(i - start), reply, sizeof reply) > 0) send_line(reply);
        start = i + 1;
    }
    memmove(buf, buf + start, blen - start);
    blen -= start;
    if (blen == sizeof buf) { /* over-long line: drop it up to its newline */
        blen = 0;
        if (!discarding) send_line("{\"op\":\"error\",\"msg\":\"line too long\"}");
        discarding = 1;
    }
}
