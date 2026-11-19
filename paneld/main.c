// paneld — phonepanel display daemon, PID 1 in the initramfs.
//
// Brings up /dev, /proc, /sys, configfs, draws the test pattern, paints touch
// points, and exposes a control channel on a USB CDC-ACM serial port.
//
// Safety net: the power key reboots to the normal boot partition; so does
// idle_reboot_s without touch or host traffic (host can change it with `idle`).
#include "paneld.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void early_setup(void) {
    if (getpid() != 1) return; /* running as a normal program (testing) */
    int devtmpfs = mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0;
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    if (!devtmpfs) {
        /* No devtmpfs in this kernel (e.g. the Galaxy S8's TWRP kernel): /dev only has what we create. Other
           nodes are made on demand (ensure_node), but tools need the memory devices up front: wpa_supplicant
           can't do a WPA handshake without /dev/urandom. */
        static const char *const mem[] = {"null", "zero", "full", "random", "urandom", "kmsg"};
        for (unsigned i = 0; i < sizeof mem / sizeof *mem; i++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/%s", mem[i]);
            ensure_node("mem", mem[i], path);
            if (strcmp(mem[i], "kmsg")) chmod(path, 0666);
        }
    }
    mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");
    mount("tmpfs", "/run", "tmpfs", 0, "mode=0755");
    mount("configfs", "/config", "configfs", 0, NULL);
    mkdir("/sys/fs/pstore", 0755);
    mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL);
    /* Keep fds 0-2 occupied, otherwise the first files we open (kmsg, the
     * control tty) become stdout/stderr and log text leaks onto the USB serial
     * line. This kernel has no console device, so fall back to /dev/null. */
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
}

/* Mode A: forward the phone's touches to the host as a USB HID touchscreen. */
static void send_hid_touch(void) {
    static int hid_fd = -1;
    static unsigned hid_gen;
    wd_phase("hid:open");
    if (hid_fd >= 0 && hid_gen != gadget_bind_gen()) { close(hid_fd); hid_fd = -1; } /* rebind: the node may be new */
    if (hid_fd < 0) { hid_fd = open("/dev/hidg0", O_WRONLY | O_NONBLOCK | O_CLOEXEC); hid_gen = gadget_bind_gen(); }
    wd_phase("hid:build");
    if (hid_fd < 0) return;
    int ids[10], xs[10], ys[10], down[10];
    int n = touch_hid_contacts(ids, xs, ys, down, 10);
    if (!n) return;
    uint8_t rep[80];
    int len = gadget_hid_report(rep, ids, xs, ys, down, n);
    wd_phase("hid:write");
    if (write(hid_fd, rep, len) < 0 && errno != EAGAIN) { close(hid_fd); hid_fd = -1; } /* reopen after a rebind */
}

static int have_touch(const struct app *a) {
    for (int i = 0; i < a->nin; i++)
        if (a->in[i].is_touch) return 1;
    return 0;
}

int main(void) {
    if (getpid() != 1 && getenv("PANELD_PRINT_CONFIG")) { hw_print_config(); return 0; } /* CI: paneld-config.txt */
    if (getpid() != 1 && getenv("PANELD_PRINT_USB_PLAN")) return gadget_plan();         /* CI: footprint.txt [usb] */
    early_setup();
    struct app a = {.idle_reboot_s = PP_AUTOREBOOT_SECS, .boot_ms = now_ms(), .clock_on = 1};
    logmsg("phonepanel paneld %s starting (pid %d, t=%ld ms)", PP_VERSION, getpid(), a.boot_ms);

    /* Outside PID 1 (e.g. `make test` on a PC) never touch real hardware unless
     * asked: PANELD_FB=/dev/fbN and PANELD_INPUT=1 opt in. */
    int hw = getpid() == 1;
    const char *fbpath = getenv("PANELD_FB");
    static struct fb fb;
    a.fb = &fb;
    a.have_fb = (hw || fbpath) && fb_open(&fb, hw ? NULL : fbpath) == 0;
    touch_init(&fb);
    if (hw) store_init(PP_STORE_PART); /* first: power, keys, modes and the gadget read their settings from it */
    if (hw && boot_key_held(KEY_VOLUMEDOWN)) {
        store_set_safe_mode(1);
        logmsg("SAFE MODE: Vol Down held at boot, saved settings ignored for this boot (nothing erased)");
    }
    if (hw) power_init(a.have_fb ? &fb : NULL);
    ui_init(&fb);
    keys_init();
    if (hw && a.have_fb) hm_init(&fb);
    panel_init(&fb);
    if (hw) net_init();
    if (a.have_fb) {
        draw_test_pattern(&fb);
        logmsg("first frame at t=%ld ms", now_ms());
        if (store_safe_mode()) ui_toast("SAFE MODE: SETTINGS IGNORED");
    }
    if (!hw && !fbpath) logmsg("dry run: not PID 1, framebuffer skipped (set PANELD_FB to use one)");

    static struct input in[MAX_INPUTS];
    a.in = in;
    a.nin = (hw || getenv("PANELD_INPUT")) ? inputs_open(in, MAX_INPUTS) : 0;

    char ttys[GADGET_PORTS][64] = {{0}};
    if (hw) {
        const char *lr = store_get("last_reboot", "");
        logmsg("previous reboot by paneld: %s", lr[0] ? lr : "none recorded (power loss, forced reset, ROM or kernel)");
        if (lr[0]) store_set("last_reboot", "");
    }
    if (hw) shell_setup();
    if (hw && gadget_setup(ttys, a.have_fb ? &fb : NULL) == 0) {
        log_port_open(ttys[GADGET_LOG]);
        ctl_open(ttys[GADGET_CTL]);
        shell_start(ttys[GADGET_SH]);
        if (a.have_fb) { pb_init(&fb); term_init(&fb); }
    } else if (getenv("PANELD_CTL")) ctl_open(getenv("PANELD_CTL")); /* testing: a pty */

    if (!a.have_fb && hw && ctl_fd() < 0) {
        logmsg("no framebuffer and no control channel; rebooting in 30 s");
        sleep(30);
        do_reboot("nothing usable", NULL);
    }

    a.last_activity_ms = now_ms();
    long ctl_quiet_until = 0, log_quiet_until = 0;
    wd_start();
    logmsg("ready at t=%ld ms; power key reboots; idle reboot after %d s", now_ms(), a.idle_reboot_s);

    for (;;) {
        wd_beat("poll");
        struct pollfd pfd[MAX_INPUTS + 6];
        int n = 0;
        for (int i = 0; i < a.nin; i++) pfd[n++] = (struct pollfd){.fd = in[i].fd, .events = POLLIN};
        int ctl_idx = -1, log_idx = -1;
        if (ctl_fd() >= 0 && now_ms() >= ctl_quiet_until) {
            ctl_idx = n;
            pfd[n++] = (struct pollfd){.fd = ctl_fd(), .events = POLLIN};
        }
        if (log_port_fd() >= 0 && now_ms() >= log_quiet_until) {
            log_idx = n;
            pfd[n++] = (struct pollfd){.fd = log_port_fd(), .events = POLLIN};
        }
        /* autorotate sensors: only while rotation = AUTO and the screen is on */
        if (hw && a.have_fb) sensors_set_running(store_get_int("rotation", -1) < 0 && !power_screen_off());
        int net_idx[2] = {-1, -1};
        for (int k = 0; k < 2; k++)
            if (net_fd(k) >= 0) { net_idx[k] = n; pfd[n++] = (struct pollfd){.fd = net_fd(k), .events = POLLIN}; }
        /* RESCUE takes the shell port while that mode is selected (also behind the menu) */
        if (hw && a.have_fb) {
            int want = gadget_mode() == 'A' && hm_input_mode() == IN_RESCUE;
            if (want != term_active()) term_activate(want, ttys[GADGET_SH]);
        }
        int term_idx = -1;
        if (term_fd() >= 0) { term_idx = n; pfd[n++] = (struct pollfd){.fd = term_fd(), .events = POLLIN}; }
        int pb_idx = -1;
        if (pb_fd() >= 0) { pb_idx = n; pfd[n++] = (struct pollfd){.fd = pb_fd(), .events = POLLIN}; }
        int adb_idx = -1;
        if (adbd_notify_fd() >= 0) { adb_idx = n; pfd[n++] = (struct pollfd){.fd = adbd_notify_fd(), .events = POLLIN}; }
        int sen_idx[2] = {-1, -1};
        for (int k = 0; k < 2; k++)
            if (sensors_fd(k) >= 0) { sen_idx[k] = n; pfd[n++] = (struct pollfd){.fd = sensors_fd(k), .events = POLLIN}; }
        /* Rescan input devices: fast while no touchscreen yet (it may probe late),
         * then every 2 s (hotplug, the injector's virtual touchscreen). */
        int touch_pending = hw && !have_touch(&a) && now_ms() - a.boot_ms < 20000;
        /* Sleep until the next wall-clock second (clock redraw) or input; nothing else
         * wakes us, so a static screen costs one tiny redraw per second. */
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        int to_next_s = 1000 - (int)(rt.tv_nsec / 1000000) + 1;
        int timeout = touch_pending ? 100 : to_next_s;
        if (hw) {
            gadget_tick();
            adbd_tick();
            pb_route_tick();
            wifi_tick();
            ssh_tick();
            bt_tick();
            term_tick();
            int pt = power_tick(), kt = keys_tick();
            if (kt < pt) pt = kt;
            if (pt < timeout) timeout = pt < 0 ? 0 : pt;
            if (power_screen_off()) timeout = touch_pending ? 100 : (pt > 0 ? pt : 1000); /* no clock while off */
        }
        if (touch_corner_pending() && timeout > 100) timeout = 100;
        if (a.have_fb) { int ft = fb_tick(&fb); if (ft >= 0 && ft < timeout) timeout = ft; }
        if (hw && a.have_fb && hm_input_mode() >= IN_KEYBOARD && timeout > 30) timeout = 30; /* DEL repeat */
        int r = poll(pfd, n, timeout);
        if (hw && touch_corner_hold(2000)) { /* on-screen menu: corner long-press toggles USB mode */
            char m[2] = {gadget_mode() == 'A' ? 'B' : 'A', 0};
            logmsg("usb: corner long-press: switching to mode %s", m);
            store_set("usb_mode", m);
            gadget_set_mode(m[0]);
        }
        if (hw && a.have_fb) { int ht = hm_tick(); (void)ht; } /* screen ownership + key repeat */
        if (a.have_fb && a.clock_on && !(hw && (power_screen_off() || hm_owner() != OWN_PAGE))) {
            static time_t shown;
            clock_gettime(CLOCK_REALTIME, &rt);
            if (rt.tv_sec != shown) {
                shown = rt.tv_sec;
                struct tm tm;
                gmtime_r(&rt.tv_sec, &tm);
                char t[32];
                snprintf(t, sizeof t, "%02d:%02d:%02d UTC", tm.tm_hour, tm.tm_min, tm.tm_sec);
                if (hw) ui_tick();
                if (ui_page() == UI_PAGE_TEST) draw_clock(&fb, t);
                if (hw) {
                    static char last[48];
                    char st[48];
                    snprintf(st, sizeof st, "USB %c %-14.14s", gadget_mode() ? gadget_mode() : '-', gadget_state());
                    if (strcmp(st, last)) {
                        snprintf(last, sizeof last, "%s", st);
                        if (strcmp(gadget_state(), "configured")) logmsg("usb: state %s", gadget_state());
                        if (ui_page() == UI_PAGE_TEST) draw_status(&fb, st);
                    }
                }
                fb_present(&fb);
            }
        }
        if (hw) shell_reap();
        static long next_scan;
        if ((hw || getenv("PANELD_INPUT")) && (touch_pending || now_ms() >= next_scan)) {
            next_scan = now_ms() + 2000;
            int had_touch = have_touch(&a), before = a.nin;
            a.nin = inputs_scan(in, a.nin, MAX_INPUTS);
            if (!had_touch && have_touch(&a)) logmsg("touch ready at t=%ld ms", now_ms());
            if (a.nin != before) continue; /* new fds: rebuild pfd[] before using revents */
        }
        if (a.idle_reboot_s > 0 && now_ms() - a.last_activity_ms > a.idle_reboot_s * 1000L)
            do_reboot("idle timeout", NULL);
        if (r <= 0) continue;

        for (int k = 0; k < 2; k++)
            if (sen_idx[k] >= 0 && (pfd[sen_idx[k]].revents & POLLIN)) sensors_read(k);
        for (int k = 0; k < 2; k++)
            if (net_idx[k] >= 0 && pfd[net_idx[k]].revents) { wd_beat("net"); net_handle(k); }
        if (pb_idx >= 0 && (pfd[pb_idx].revents & POLLIN)) { wd_beat("phonebox"); pb_handle(); }
        if (term_idx >= 0 && pfd[term_idx].revents) { wd_beat("rescue"); term_read(); }
        if (adb_idx >= 0 && (pfd[adb_idx].revents & POLLIN)) {
            /* the phonebox agent's adb commands: input keyevent WAKEUP/SLEEP/POWER, am start (open the panel) */
            for (int c; (c = adbd_notify_read());) {
                if (c == 'w') power_wake("host (adb)");
                else if (c == 's') power_off("host (adb)");
                else if (c == 'p') power_toggle();
                else if (c == 'o' && hw && a.have_fb) { power_wake("host opened the panel"); hm_show_phonebox(); }
            }
        }
        if (log_idx >= 0 && pfd[log_idx].revents) {
            /* a hung-up tty reports POLLIN|POLLHUP and reads EOF: reopen for a fresh one */
            if (pfd[log_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) { log_port_reopen(); log_quiet_until = now_ms() + 300; }
            else log_port_drain();
        }
        if (ctl_idx >= 0 && pfd[ctl_idx].revents) {
            wd_beat("ctl");
            if (pfd[ctl_idx].revents & (POLLHUP | POLLERR | POLLNVAL)) { ctl_reopen(); ctl_quiet_until = now_ms() + 300; }
            else if (ctl_handle(&a) < 0) { ctl_reopen(); ctl_quiet_until = now_ms() + 300; } /* EOF/EIO */
        }
        for (int i = 0; i < a.nin; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            wd_beat("input");
            /* Drain everything pending, draw all of it, flush ONCE. A flush waits for
             * vsync (~16.7 ms) while touch reports come every ~8 ms, so flushing per
             * report builds a queue (measured: 250 ms average lag). */
            struct input_event ev[64], first_syn = {0};
            int dirty = 0;
            ssize_t got;
            while ((got = read(in[i].fd, ev, sizeof ev)) > 0) {
                for (int k = 0; k < (int)(got / (ssize_t)sizeof ev[0]); k++) {
                    struct input_event *e = &ev[k];
                    a.last_activity_ms = now_ms();
                    if (e->type == EV_KEY && !in[i].is_touch) {
                        if (hw) keys_event(e->code, e->value); /* configurable actions; power held 3 s: reboot */
                        else if (e->code == KEY_POWER && e->value == 1) do_reboot("power key", NULL);
                    }
                    if (in[i].is_touch && hw && e->type == EV_SYN) power_user_activity();
                    if (in[i].is_touch && touch_event(&in[i], e)) {
                        /* Dispatch every report (not once per read): a down, moves and a lift
                         * can arrive in one read, and UIs must see each state, or the lift is lost. */
                        int consumed = hw && a.have_fb && hm_touch_frame();
                        if (!consumed && hw && gadget_mode() == 'A') send_hid_touch();
                        if (!consumed) {
                            if (!dirty) first_syn = *e; /* oldest report in this flush = worst case */
                            dirty = 1;
                        }
                    }
                }
            }
            if (dirty && a.have_fb && !(hw && hm_owner() != OWN_PAGE)) {
                wd_beat("fb_present");
                if (fb_present(&fb)) touch_flushed(&first_syn);
            }
        }
    }
}
