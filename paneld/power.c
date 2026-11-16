// Power manager: screen state machine, backlight, CPU caps, charge limiter, thermal guard.
//
// Screen: ACTIVE --dim_after_s--> DIM --off_after_s--> OFF (FBIOBLANK powerdown).
//   Only real user input (touch, keys) counts as activity. Touch wakes from DIM;
//   in OFF the touch controller is asleep (the TDDI chip goes down with the panel),
//   so keys or the host (`power wake`) wake it. Power key: short press toggles the
//   screen, holding it 3 s reboots.
// Battery: charging is switched off at charge_hi % and back on at charge_lo %
//   (USB keeps powering the phone, battery current ~0). Above temp_limit charging
//   stops; above temp_crit the screen is forced off and CPUs capped.
#include "paneld.h"

#include <dirent.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BATT "/sys/class/power_supply/battery"

struct power_cfg pcfg = {
    .dim_after_s = 0, .off_after_s = 0, .bright = 128, .dim = 12, /* never by default: opt in via Settings */
    .charge_lo = 40, .charge_hi = 60, .temp_limit = 450, .temp_crit = 550, /* 0.1 °C */
    .cap_cpu_when_off = 1, .limiter = 1,
};

static enum { ACTIVE, DIM, OFF } state = ACTIVE;
static const char *const state_name[] = {"active", "dim", "off"};
static char bl_path[160];
static long user_ms, last_batt_ms = -100000;
static int charging_allowed = -1, forced_off;
static struct fb *fb;
static int last_cap = -1, last_temp = -1;
static long wake_ms_last = -1;

static int read_int(const char *path, int dflt) {
    char b[32];
    return read_file(path, b, sizeof b) > 0 ? atoi(b) : dflt;
}

static void write_int(const char *path, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    write_file(path, b);
}

static void set_backlight(int v) {
    if (bl_path[0]) write_int(bl_path, v);
}

/* cap: 1 = clamp every cpufreq policy to its minimum, 0 = restore the maximum */
static void cpu_cap(int cap) {
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "policy", 6)) continue;
        char p[128], v[32];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/%.32s/%s", e->d_name,
                 cap ? "cpuinfo_min_freq" : "cpuinfo_max_freq");
        if (read_file(p, v, sizeof v) <= 0) continue;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/%.32s/scaling_max_freq", e->d_name);
        write_file(p, v);
    }
    closedir(d);
}

static void enter(int s, const char *why) {
    if (s == (int)state) return;
    long t0 = now_ms();
    if (s == OFF) {
        set_backlight(0);
        if (fb) fb_blank(fb, 1);
        if (pcfg.cap_cpu_when_off) cpu_cap(1);
    } else {
        if (state == OFF) {
            if (pcfg.cap_cpu_when_off) cpu_cap(0);
            if (fb) fb_blank(fb, 0);
        }
        set_backlight(s == DIM ? pcfg.dim : pcfg.bright);
    }
    if (state == OFF && s != OFF) wake_ms_last = now_ms() - t0;
    logmsg("power: %s -> %s (%s)%s", state_name[state], state_name[s], why,
           state == OFF && s != OFF ? "" : "");
    state = s;
}

void power_init(struct fb *f) {
    fb = f;
    DIR *d = opendir("/sys/class/backlight");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.') {
                snprintf(bl_path, sizeof bl_path, "/sys/class/backlight/%.64s/brightness", e->d_name);
                break;
            }
        closedir(d);
    }
    user_ms = now_ms();
    pcfg.bright = store_get_int("power.bright", pcfg.bright);
    pcfg.dim_after_s = store_get_int("power.dim_after_s", pcfg.dim_after_s);
    pcfg.off_after_s = store_get_int("power.off_after_s", pcfg.off_after_s);
    pcfg.limiter = store_get_int("power.limiter", pcfg.limiter);
    pcfg.charge_lo = store_get_int("power.charge_lo", pcfg.charge_lo);
    pcfg.charge_hi = store_get_int("power.charge_hi", pcfg.charge_hi);
    set_backlight(pcfg.bright);
    logmsg("power: backlight %s, dim after %d s, off after %d s, charge window %d-%d %%",
           bl_path[0] ? bl_path : "(none)", pcfg.dim_after_s, pcfg.off_after_s, pcfg.charge_lo, pcfg.charge_hi);
}

void power_user_activity(void) {
    user_ms = now_ms();
    if (state == DIM && !forced_off) enter(ACTIVE, "input");
}

void power_toggle(void) {
    user_ms = now_ms();
    if (state == OFF) { if (!forced_off) enter(ACTIVE, "toggle"); }
    else enter(OFF, "toggle");
}

void power_off(const char *why) { enter(OFF, why); }
void power_dim(const char *why) { if (state == ACTIVE) enter(DIM, why); }

void power_wake(const char *why) {
    user_ms = now_ms();
    if (!forced_off) enter(ACTIVE, why);
}

void power_set_percent(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    /* perceptual -> backlight: quadratic, 0 % stays faintly on (GUD semantics) */
    int v = 4 + (int)((long)pct * pct * (306 - 4) / 10000);
    pcfg.bright = v;
    if (state == ACTIVE) set_backlight(v);
    logmsg("power: brightness %d %% -> %d", pct, v);
}

/* Brightness in ~10 perceptual steps (backlight current is far from linear). */
void power_bright_step(int dir) {
    static const int steps[] = {4, 8, 16, 32, 56, 90, 128, 170, 220, 306};
    int n = sizeof steps / sizeof *steps, i = 0;
    while (i < n - 1 && steps[i] < pcfg.bright) i++;
    i += dir;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    pcfg.bright = steps[i];
    char v[8];
    snprintf(v, sizeof v, "%d", pcfg.bright);
    store_set("power.bright", v);
    if (state == ACTIVE) set_backlight(pcfg.bright);
    logmsg("power: brightness %d", pcfg.bright);
}

static void battery_tick(void) {
    int cap = read_int(BATT "/capacity", -1), temp = read_int(BATT "/batt_temp", -1);
    last_cap = cap; last_temp = temp;
    int allow = charging_allowed;
    if (temp >= pcfg.temp_limit) allow = 0;
    else if (!pcfg.limiter) allow = 1;
    else if (cap >= pcfg.charge_hi) allow = 0;
    else if (cap >= 0 && cap <= pcfg.charge_lo) allow = 1;
    else if (allow < 0) allow = cap < pcfg.charge_hi; /* first run: inside the window */
    /* re-assert every tick: the battery driver may re-enable charging on cable events */
    if (allow >= 0) write_int(BATT "/charging_enabled", allow);
    if (allow != charging_allowed)
        logmsg("power: charging %s (battery %d %%, %d.%d C)", allow ? "on" : "off", cap, temp / 10, temp % 10);
    charging_allowed = allow;

    if (temp >= pcfg.temp_crit && !forced_off) {
        logmsg("power: battery %d.%d C >= critical %d.%d C: screen off, CPUs capped", temp / 10, temp % 10,
               pcfg.temp_crit / 10, pcfg.temp_crit % 10);
        forced_off = 1;
        enter(OFF, "overheat");
    } else if (forced_off && temp < pcfg.temp_crit - 30) {
        logmsg("power: battery cooled to %d.%d C", temp / 10, temp % 10);
        forced_off = 0;
    }
}

/* Call every loop iteration; returns the ms until it wants to run again. */
int power_tick(void) {
    long now = now_ms();
    if (now - last_batt_ms >= 30000) { last_batt_ms = now; battery_tick(); }
    long idle = now - user_ms;
    if (state == ACTIVE && pcfg.dim_after_s > 0 && idle >= pcfg.dim_after_s * 1000L) enter(DIM, "idle");
    if (state != OFF && pcfg.off_after_s > 0 && idle >= pcfg.off_after_s * 1000L) enter(OFF, "idle");
    return 30000 - (int)(now - last_batt_ms);
}

int power_screen_off(void) { return state == OFF; }

/* ctl: power [status] | power wake | power off | power set <key> <value> */
int power_ctl(char **argv, int argc, char *buf, size_t len) {
    if (argc >= 2 && !strcmp(argv[1], "wake")) { forced_off = 0; user_ms = now_ms(); enter(ACTIVE, "host"); }
    else if (argc >= 2 && !strcmp(argv[1], "off")) enter(OFF, "host");
    else if (argc >= 2 && !strcmp(argv[1], "tick")) { last_batt_ms = now_ms(); battery_tick(); }
    else if (argc >= 4 && !strcmp(argv[1], "set")) {
        int v = atoi(argv[3]);
        struct { const char *k; int *p; } keys[] = {
            {"dim_after_s", &pcfg.dim_after_s}, {"off_after_s", &pcfg.off_after_s},
            {"bright", &pcfg.bright}, {"dim", &pcfg.dim}, {"charge_lo", &pcfg.charge_lo},
            {"charge_hi", &pcfg.charge_hi}, {"temp_limit", &pcfg.temp_limit},
            {"temp_crit", &pcfg.temp_crit}, {"cap_cpu_when_off", &pcfg.cap_cpu_when_off},
            {"limiter", &pcfg.limiter}};
        int found = 0;
        for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
            if (!strcmp(argv[2], keys[i].k)) { *keys[i].p = v; found = 1; }
        if (!found) return -1;
        if (!strcmp(argv[2], "bright") && state == ACTIVE) set_backlight(v);
        if (!strcmp(argv[2], "dim") && state == DIM) set_backlight(v);
        logmsg("power: %s = %d", argv[2], v);
    }
    return snprintf(buf, len,
                    "state %s\nidle_ms %ld\nlast_wake_ms %ld\nbacklight %d\n"
                    "dim_after_s %d\noff_after_s %d\nbright %d\ndim %d\n"
                    "battery_pct %d\nbattery_temp_dC %d\ncharging_allowed %d\nlimiter %d\ncharge_lo %d\ncharge_hi %d\n"
                    "temp_limit_dC %d\ntemp_crit_dC %d\nforced_off %d\nbattery_current_ua %d\n",
                    state_name[state], now_ms() - user_ms, wake_ms_last,
                    bl_path[0] ? read_int(bl_path, -1) : -1, pcfg.dim_after_s, pcfg.off_after_s, pcfg.bright,
                    pcfg.dim, last_cap, last_temp, charging_allowed, pcfg.limiter, pcfg.charge_lo,
                    pcfg.charge_hi, pcfg.temp_limit, pcfg.temp_crit, forced_off,
                    read_int(BATT "/batt_current_ua_now", 0));
}
