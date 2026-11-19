// Bluetooth on a Broadcom UART controller (hw.c .bt = HW_BT_BCM_UART, Galaxy S9+ / BCM4361). Does what Android's
// libbt-vendor does before its stack starts: power the chip through rfkill, open the UART at 115200 8N1 with
// RTS/CTS, HCI reset, download the .hcd patchram (a list of HCI vendor commands ending with Launch RAM), reset
// again, then switch the chip and the host to the fast rate. bt.c then bridges the tty to /dev/vhci exactly as it
// bridges Samsung's SCSC H4 device, so the kernel's Bluetooth core gets hci0.
#include "paneld.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define BCM_FAST_BAUD 3000000

static void rfkill_set(int on) {
    DIR *d = opendir("/sys/class/rfkill");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[128], n[64] = "";
        snprintf(p, sizeof p, "/sys/class/rfkill/%.32s/name", e->d_name);
        if (read_file(p, n, sizeof n) < 0) continue;
        n[strcspn(n, "\n")] = 0;
        if (strcmp(n, phone.bt_rfkill)) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/%.32s/state", e->d_name);
        write_file(p, on ? "1" : "0");
    }
    closedir(d);
}

static int set_speed(int fd, speed_t sp) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    cfmakeraw(&t);
    t.c_cflag |= CRTSCTS | CLOCAL | CREAD;
    t.c_cflag &= ~CSTOPB;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    tcflush(fd, TCIOFLUSH);
    return 0;
}

/* Send one HCI command, wait for its Command Complete (or Command Status). 0 = status 0x00. */
static int hci_cmd(int fd, unsigned op, const unsigned char *p, unsigned len, int timeout_ms) {
    unsigned char b[3 + 1 + 255];
    b[0] = 0x01; b[1] = op & 0xff; b[2] = op >> 8; b[3] = (unsigned char)len;
    if (len) memcpy(b + 4, p, len);
    for (size_t off = 0, n = 4 + len; off < n;) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    unsigned char in[300];
    size_t have = 0;
    long until = now_ms() + timeout_ms;
    while (now_ms() < until) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 50) <= 0) continue;
        ssize_t r = read(fd, in + have, sizeof in - have);
        if (r <= 0) continue;
        have += (size_t)r;
        while (have && in[0] != 0x04) memmove(in, in + 1, --have); /* only events come back here */
        if (have < 3 || have < 3u + in[2]) continue;
        unsigned ev = in[1], plen = in[2];
        if (ev == 0x0e && plen >= 4 && (unsigned)(in[4] | in[5] << 8) == op) return in[6];
        if (ev == 0x0f && plen >= 4 && (unsigned)(in[5] | in[6] << 8) == op) return in[3];
        memmove(in, in + 3 + plen, have - 3 - plen); /* some other event: drop it */
        have -= 3 + plen;
    }
    return -2;
}

/* The .hcd file: records of opcode (LE16), length, parameters. Sent as-is after Download Minidriver. */
static int send_patchram(int fd) {
    int f = open(phone.bt_fw, O_RDONLY | O_CLOEXEC);
    if (f < 0) { logmsg("bt: %s: %s", phone.bt_fw, strerror(errno)); return -1; }
    int rc = hci_cmd(fd, 0xfc2e, NULL, 0, 2000); /* Download Minidriver */
    if (rc) { logmsg("bt: download minidriver failed (%d)", rc); close(f); return -1; }
    usleep(50000);
    unsigned char h[3], p[255];
    int n = 0;
    while (read(f, h, 3) == 3) {
        unsigned op = h[0] | h[1] << 8, len = h[2];
        if (len && read(f, p, len) != (ssize_t)len) break;
        rc = hci_cmd(fd, op, p, len, 2000);
        if (op == 0xfc4e) break; /* Launch RAM: the chip restarts, often without answering */
        if (rc) { logmsg("bt: patchram record %d (0x%04x) failed (%d)", n, op, rc); close(f); return -1; }
        n++;
    }
    close(f);
    logmsg("bt: patchram %s: %d records", phone.bt_fw, n);
    usleep(250000);
    return 0;
}

/* Broadcom Update UART Baud Rate (0xfc18), then the host follows. 0 when the chip answers at the new rate. */
static int fast_baud(int fd) {
    unsigned char b[6] = {0, 0, BCM_FAST_BAUD & 0xff, (BCM_FAST_BAUD >> 8) & 0xff, (BCM_FAST_BAUD >> 16) & 0xff,
                          (unsigned char)(BCM_FAST_BAUD >> 24)};
    if (hci_cmd(fd, 0xfc18, b, sizeof b, 2000)) return -1;
    set_speed(fd, B3000000);
    return hci_cmd(fd, 0x0c03, NULL, 0, 2000) ? -1 : 0;
}

/* The phone's own address, as Android sets it from /efs/bluetooth/bt_addr ("aa:bb:cc:dd:ee:ff"; the image carries a
   copy there, from prebuilt/firmware/efs-root/), else the chip keeps Broadcom's shared default 43:61:B2:00:1F:AC. */
static void set_bdaddr(int fd) {
    char a[32] = "";
    unsigned u[6];
    if (read_file("/efs/bluetooth/bt_addr", a, sizeof a) < 0 ||
        sscanf(a, "%2x:%2x:%2x:%2x:%2x:%2x", &u[0], &u[1], &u[2], &u[3], &u[4], &u[5]) != 6) {
        logmsg("bt: no bt_addr in the image: Broadcom's default address");
        return;
    }
    unsigned char le[6];
    for (int i = 0; i < 6; i++) le[i] = (unsigned char)u[5 - i];
    if (hci_cmd(fd, 0xfc01, le, sizeof le, 2000)) logmsg("bt: write BD_ADDR failed");
}

int bt_bcm_open(void) {
    char dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", phone.bt_uart);
    ensure_node("tty", phone.bt_uart, dev);
    rfkill_set(0);
    usleep(100000);
    rfkill_set(1);
    usleep(300000);
    int fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) { logmsg("bt: %s: %s", dev, strerror(errno)); rfkill_set(0); return -1; }
    long t0 = now_ms();
    int rc = -1;
    /* as libbt-vendor: reset, fast rate for the ~100 KB patch, patch; the patched firmware restarts at 115200 */
    if (set_speed(fd, B115200) == 0 && (rc = hci_cmd(fd, 0x0c03, NULL, 0, 2000)) == 0) {
        if (fast_baud(fd)) { logmsg("bt: fast rate refused before the patch, downloading at 115200"); set_speed(fd, B115200); }
        if (send_patchram(fd) == 0) {
            set_speed(fd, B115200);
            rc = hci_cmd(fd, 0x0c03, NULL, 0, 2000);
            if (!rc && fast_baud(fd) == 0) {
                set_bdaddr(fd);
                logmsg("bt: Broadcom controller up on %s at %d baud in %ld ms", dev, BCM_FAST_BAUD, now_ms() - t0);
                return fd;
            }
            logmsg("bt: no answer after switching to %d baud", BCM_FAST_BAUD);
        }
    }
    logmsg("bt: Broadcom bring-up failed on %s (%d)", dev, rc);
    close(fd);
    rfkill_set(0);
    return -1;
}

void bt_bcm_close(int fd) {
    close(fd);
    rfkill_set(0);
}
