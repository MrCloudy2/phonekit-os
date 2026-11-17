// Wi-Fi client (M10): joins the network stored in the settings store with wpa_supplicant (WPA2-Personal) and gets
// an address with busybox udhcpc. Nothing runs unless a network is configured, so it costs no power otherwise.
//
//   ppctl wifi                      status
//   ppctl wifi scan                 nearby networks (iw)
//   ppctl wifi set "<ssid>" "<psk>" store a network and join it (the user types the password; it stays on the phone)
//   ppctl wifi off                  forget it and stop
//   ppctl wifi radio on|off         Wi-Fi on/off without forgetting the network (default on)
//   ppctl wifi wired-off on|off     keep Wi-Fi off while a computer is on the USB cable (default on; WI-FI MODEM
//                                   always keeps it on)
// Settings -> WI-FI on the phone does the same, and scans + takes the password on screen (netui.c).
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

/* Should Wi-Fi run now? A network is set up, the radio is on, and (unless WI-FI MODEM needs it) no computer is on
 * the cable while "keep off while wired" is on. Everything the user doesn't need stays off. */
int wifi_wanted(void) {
    if (!store_get("wifi.ssid", "")[0] || !store_get_int("wifi.on", 1)) return 0;
    if (store_get_int("tether", 0)) return 1;
    if (!store_get_int("wifi.wired_off", 1)) return 1;
    return gadget_wired_known() && !gadget_wired(); /* at boot: wait until we know whether a computer is there */
}

/* Main loop, about once a second: (re)start when wanted, stop when not, restart crashed helpers. */
void wifi_tick(void) {
    static int last_want = -1;
    int want = wifi_wanted();
    if (want != last_want) { next_try = 0; if (last_want >= 0) logmsg("wifi: %s", want ? "on" : "off (not needed now)"); last_want = want; }
    if (now_ms() < next_try) return;
    next_try = now_ms() + 5000;
    char ip[32] = "";
    int have_ip = want && wpa_pid > 0 && read_file("/run/wifi.ip", ip, sizeof ip) >= 0 && ip[0];
    tether_apply(store_get_int("tether", 0) && have_ip);
    if (!want) {
        if (wpa_pid > 0 || dhcp_pid > 0) { stop_all(); unlink("/run/wifi.ip"); }
        return;
    }
    if (wpa_pid > 0 && waitpid(wpa_pid, NULL, WNOHANG) == wpa_pid) wpa_pid = -1;
    if (dhcp_pid > 0 && waitpid(dhcp_pid, NULL, WNOHANG) == dhcp_pid) dhcp_pid = -1;
    if (wpa_pid < 0 || dhcp_pid < 0) { stop_all(); start(); }
}

int wifi_tether_active(void) { return nat_on; }

int wifi_running(void) { return wpa_pid > 0; }

/* Joins a network (Settings -> WI-FI, or ppctl wifi set). Returns NULL, or what's wrong. */
const char *wifi_join(const char *ssid, const char *psk) {
    if (!safe(ssid) || (psk[0] && !safe(psk))) return "quotes and backslashes aren't supported";
    if (psk[0] && (strlen(psk) < 8 || strlen(psk) > 63)) return "a WPA2 password has 8 to 63 characters";
    store_set("wifi.ssid", ssid);
    store_set("wifi.psk", psk);
    store_set("wifi.on", "1");
    unlink("/run/wifi.ip");
    stop_all();
    next_try = 0;
    logmsg("wifi: joining a new network from %s", "settings");
    return NULL;
}

void wifi_forget(void) {
    store_set("wifi.ssid", "");
    store_set("wifi.psk", "");
    unlink("/run/wifi.ip");
    stop_all();
}

/* The Wi-Fi screen powers the chip up to scan even when Wi-Fi is off; this puts it back down afterwards. */
void wifi_radio_idle(void) {
    if (wpa_pid <= 0) { char *down[] = {"/bin/busybox", "ifconfig", "wlan0", "down", NULL}; run_quiet(down); }
}

/* ssid (empty if not set up) and the phone's Wi-Fi IP; returns 1 when it has an address. */
int wifi_status(char *ssid, size_t sl, char *ip, size_t il) {
    snprintf(ssid, sl, "%s", store_get("wifi.ssid", ""));
    if (read_file("/run/wifi.ip", ip, il) < 0) ip[0] = 0;
    for (char *p = ip; *p; p++) if (*p == '\n') *p = 0;
    return ssid[0] && ip[0];
}

/* Tethering on/off; replug = make the PC take the new DHCP offer now (else at its next renewal). */
void wifi_tether_set(int on, int replug) {
    if (store_get_int("tether", 0) == on) return;
    store_set("tether", on ? "1" : "0");
    next_try = 0;
    wifi_tick();
    gadget_dhcp_refresh();
    if (replug && gadget_mode() == 'B') gadget_reconnect(800);
}

int wifi_ctl(char **argv, int argc, char *out, size_t len) {
    if (argc > 1 && !strcmp(argv[1], "set") && argc > 2) {
        const char *bad = wifi_join(argv[2], argc > 3 ? argv[3] : "");
        if (bad) return snprintf(out, len, "error: %s\n", bad);
        return snprintf(out, len, "joining '%s'%s\n", argv[2], wifi_wanted() ? "" : " (when Wi-Fi is needed: a computer is on the cable)");
    }
    if (argc > 1 && !strcmp(argv[1], "off")) {
        wifi_forget();
        return snprintf(out, len, "wifi off\n");
    }
    if (argc > 2 && (!strcmp(argv[1], "radio") || !strcmp(argv[1], "wired-off"))) {
        int on = !strcmp(argv[2], "on");
        store_set(argv[1][0] == 'r' ? "wifi.on" : "wifi.wired_off", on ? "1" : "0");
        next_try = 0;
        return snprintf(out, len, "%s %s\n", argv[1], on ? "on" : "off");
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
    return snprintf(out, len, "configured %s\nssid %s\nradio %s\nwired_off %s\nwired %d\nrunning %s\nwpa_supplicant %d\nudhcpc %d\nip %s\ntether %s\ntools %s\n",
                    store_get("wifi.ssid", "")[0] ? "yes" : "no", store_get("wifi.ssid", ""),
                    store_get_int("wifi.on", 1) ? "on" : "off", store_get_int("wifi.wired_off", 1) ? "on" : "off", gadget_wired(),
                    wifi_wanted() ? "yes" : "no (not needed now)", wpa_pid, dhcp_pid,
                    ip[0] ? ip : "-", nat_on ? "on (NAT active)" : store_get_int("tether", 0) ? "on (waiting for Wi-Fi)" : "off", have_tools() ? "yes" : "missing (scripts/build-wifi-tools.sh)");
}
