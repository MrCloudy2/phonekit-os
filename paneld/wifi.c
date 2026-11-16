// Wi-Fi client (M10): joins the network stored in the settings store with wpa_supplicant (WPA2-Personal) and gets
// an address with busybox udhcpc. Nothing runs unless a network is configured, so it costs no power otherwise.
//
//   ppctl wifi                      status
//   ppctl wifi scan                 nearby networks (iw)
//   ppctl wifi set "<ssid>" "<psk>" store a network and join it (the user types the password; it stays on the phone)
//   ppctl wifi off                  forget it and stop
#include "paneld.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define WPA "/bin/wpa_supplicant"
static pid_t wpa_pid = -1, dhcp_pid = -1;
static long next_try;

static int have_tools(void) { return access(WPA, X_OK) == 0 && access("/bin/busybox", X_OK) == 0; }

static pid_t spawn(char *const argv[]) {
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        setsid();
        execv(argv[0], argv);
        _exit(127);
    }
    return p;
}

static void stop_all(void) {
    if (dhcp_pid > 0) { kill(dhcp_pid, SIGTERM); dhcp_pid = -1; }
    if (wpa_pid > 0) { kill(wpa_pid, SIGTERM); wpa_pid = -1; }
    char *down[] = {"/bin/busybox", "ifconfig", "wlan0", "0.0.0.0", "down", NULL};
    run_quiet(down);
}

/* wpa_supplicant.conf quoting: refuse quotes/newlines rather than escape them */
static int safe(const char *s) { return s[0] && !strpbrk(s, "\"\n\r\\"); }

static int write_conf(const char *ssid, const char *psk) {
    mkdir("/run/wpa", 0700);
    int fd = open("/run/wpa.conf", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    char b[512];
    int n = snprintf(b, sizeof b,
                     "ctrl_interface=/run/wpa\nupdate_config=0\nap_scan=1\n"
                     "network={\n\tssid=\"%s\"\n\t%s%s%s\n\tscan_ssid=1\n}\n",
                     ssid, psk[0] ? "psk=\"" : "key_mgmt=NONE", psk[0] ? psk : "", psk[0] ? "\"" : "");
    int ok = write(fd, b, (size_t)n) == n;
    close(fd);
    /* udhcpc's hook: configure the address, route and DNS it gets */
    fd = open("/run/udhcpc.sh", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0755);
    if (fd >= 0) {
        const char *sh =
            "#!/bin/sh\n"
            "case \"$1\" in\n"
            "deconfig) ifconfig \"$interface\" 0.0.0.0 ;;\n"
            "bound|renew) ifconfig \"$interface\" \"$ip\" netmask \"${subnet:-255.255.255.0}\"\n"
            "  [ -n \"$router\" ] && { route del default dev \"$interface\" 2>/dev/null; route add default gw ${router%% *} dev \"$interface\"; }\n"
            "  [ -n \"$dns\" ] && { : > /run/resolv.conf; for d in $dns; do echo \"nameserver $d\" >> /run/resolv.conf; done; }\n"
            "  echo \"$ip\" > /run/wifi.ip ;;\n"
            "esac\n";
        ok &= write(fd, sh, strlen(sh)) == (ssize_t)strlen(sh);
        close(fd);
    }
    return ok ? 0 : -1;
}

static void start(void) {
    const char *ssid = store_get("wifi.ssid", ""), *psk = store_get("wifi.psk", "");
    if (!ssid[0] || !have_tools()) return;
    if (write_conf(ssid, psk) < 0) { logmsg("wifi: can't write config"); return; }
    char *up[] = {"/bin/busybox", "ifconfig", "wlan0", "up", NULL};
    run_quiet(up); /* loads the chip's firmware (/vendor/etc/wifi) */
    char *wa[] = {WPA, "-i", "wlan0", "-c", "/run/wpa.conf", "-D", "nl80211", NULL};
    wpa_pid = spawn(wa);
    char *dh[] = {"/bin/busybox", "udhcpc", "-f", "-i", "wlan0", "-s", "/run/udhcpc.sh", "-t", "0", "-T", "3", "-A", "5", NULL};
    dhcp_pid = spawn(dh);
    logmsg("wifi: joining '%s' (wpa_supplicant %d, udhcpc %d)", ssid, wpa_pid, dhcp_pid);
}

/* Tethering (M10b): forward the USB network (10.42.0.0/24) out of wlan0 with NAT while Wi-Fi has an address. */
static int nat_on;
static void tether_apply(int want) {
    if (want == nat_on || access("/bin/iptables", X_OK) != 0) return;
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { (void)!write(fd, want ? "1" : "0", 1); close(fd); }
    char *rule[] = {"/bin/iptables", "-t", "nat", want ? "-A" : "-D", "POSTROUTING", "-s", "10.42.0.0/24", "-o", "wlan0", "-j", "MASQUERADE", NULL};
    run_quiet(rule);
    nat_on = want;
    logmsg("wifi: tethering %s", want ? "on (USB network -> wlan0, NAT)" : "off");
}

/* Main loop, about once a second: (re)start when configured, restart crashed helpers. */
void wifi_tick(void) {
    if (now_ms() < next_try) return;
    next_try = now_ms() + 5000;
    int configured = store_get("wifi.ssid", "")[0] != 0;
    char ip[32] = "";
    int have_ip = configured && read_file("/run/wifi.ip", ip, sizeof ip) >= 0 && ip[0];
    tether_apply(store_get_int("tether", 0) && have_ip);
    if (!configured) { if (wpa_pid > 0) stop_all(); return; }
    if (wpa_pid > 0 && waitpid(wpa_pid, NULL, WNOHANG) == wpa_pid) wpa_pid = -1;
    if (dhcp_pid > 0 && waitpid(dhcp_pid, NULL, WNOHANG) == dhcp_pid) dhcp_pid = -1;
    if (wpa_pid < 0 || dhcp_pid < 0) { stop_all(); start(); }
}

int wifi_ctl(char **argv, int argc, char *out, size_t len) {
    if (argc > 1 && !strcmp(argv[1], "set") && argc > 2) {
        const char *psk = argc > 3 ? argv[3] : "";
        if (!safe(argv[2]) || (psk[0] && !safe(psk))) return snprintf(out, len, "error: quotes, backslashes and newlines aren't supported\n");
        if (psk[0] && (strlen(psk) < 8 || strlen(psk) > 63)) return snprintf(out, len, "error: a WPA2 password has 8-63 characters\n");
        store_set("wifi.ssid", argv[2]);
        store_set("wifi.psk", psk);
        stop_all();
        next_try = 0;
        return snprintf(out, len, "joining '%s'\n", argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "off")) {
        store_set("wifi.ssid", "");
        store_set("wifi.psk", "");
        stop_all();
        return snprintf(out, len, "wifi off\n");
    }
    if (argc > 1 && !strcmp(argv[1], "scan")) return -2; /* ctl runs `iw` (it can take a few seconds) */
    if (argc > 2 && !strcmp(argv[1], "tether")) {
        int on = !strcmp(argv[2], "on");
        store_set("tether", on ? "1" : "0");
        next_try = 0;
        gadget_dhcp_refresh();
        return snprintf(out, len, "tethering %s (the PC picks up the new DHCP offer at its next renewal or replug)\n", on ? "on" : "off");
    }
    char ip[32] = "";
    if (read_file("/run/wifi.ip", ip, sizeof ip) < 0) ip[0] = 0;
    return snprintf(out, len, "configured %s\nssid %s\nwpa_supplicant %d\nudhcpc %d\nip %s\ntether %s\ntools %s\n",
                    store_get("wifi.ssid", "")[0] ? "yes" : "no", store_get("wifi.ssid", ""), wpa_pid, dhcp_pid,
                    ip[0] ? ip : "-", nat_on ? "on (NAT active)" : store_get_int("tether", 0) ? "on (waiting for Wi-Fi)" : "off", have_tools() ? "yes" : "missing (scripts/build-wifi-tools.sh)");
}
