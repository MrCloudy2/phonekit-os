// Sensor hub (Samsung SSP on the Exynos CHUB co-processor) and autorotate.
//
// The CHUB firmware is built into the kernel (firmware/sensorhub/*.fw); Android powers the
// co-processor on by writing its `poweron` sysfs attribute, after which the SSP driver
// registers IIO devices per sensor. Phones whose SSP hub is a separate MCU (Galaxy S8 / Exynos 8895)
// have no contexthub device: the kernel brings the hub up and the IIO devices exist from boot. We use:
//   - device_orientation (type 27): orientation computed on the co-processor, event on
//     change only. Preferred: zero main-CPU work.
//   - accelerometer (type 1, ~6 Hz): fallback; we derive the orientation with hysteresis.
// Sensors run only while rotation = AUTO and the screen is on.
#include "paneld.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SSP_ENABLE "/sys/class/sensors/ssp_sensor/enable"
#define T_ACCEL 1
#define T_ORIENT 27
#define G_UNITS 4096      /* accelerometer: ~4096 per g (lying flat read z ~4194) */
#define STABLE_MS 600

static int powered, running, hub_failed;
static int acc_fd = -1, ori_fd = -1;
static char acc_dev[64], ori_dev[64];
static int candidate = -1, current = -1;
static long cand_since;
static unsigned long acc_samples, ori_events;

static int find_iio(const char *name, char *out, size_t len) {
    DIR *d = opendir("/sys/bus/iio/devices");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found < 0) {
        if (strncmp(e->d_name, "iio:device", 10)) continue;
        char p[300], v[64];
        snprintf(p, sizeof p, "/sys/bus/iio/devices/%s/name", e->d_name);
        if (read_file(p, v, sizeof v) > 0 && !strcmp(v, name)) {
            snprintf(out, len, "/dev/%.40s", e->d_name);
            ensure_node("iio", e->d_name, out); /* class dir is /sys/bus/iio here; devtmpfs normally has it */
            found = 0;
        }
    }
    closedir(d);
    return found;
}

static void ssp_enable(int type, int on) {
    char v[16];
    snprintf(v, sizeof v, "%d", type * 10 + (on ? 1 : 0));
    write_file(SSP_ENABLE, v);
}

static int open_stream(const char *dev) {
    char p[160];
    const char *n = strrchr(dev, '/') + 1;
    snprintf(p, sizeof p, "/sys/bus/iio/devices/%s/scan_elements/in_timestamp_en", n);
    write_file(p, "1");
    snprintf(p, sizeof p, "/sys/bus/iio/devices/%s/buffer/enable", n);
    write_file(p, "1");
    return open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* The co-processor's poweron attribute (/sys/devices/platform/<addr>.contexthub/poweron), if this phone has one. */
static int find_chub(char *out, size_t len) {
    DIR *d = opendir("/sys/devices/platform");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found < 0) {
        const char *dot = strstr(e->d_name, ".contexthub");
        if (!dot || dot[11]) continue;
        snprintf(out, len, "/sys/devices/platform/%.64s/poweron", e->d_name);
        if (access(out, W_OK) == 0) found = 0;
    }
    closedir(d);
    return found;
}

/* Powers the co-processor if there is one (once per boot); returns 0 when the sensor devices exist.
   Failures are logged once; the caller may retry quietly. */
static int hub_up(void) {
    if (powered) return 0;
    char chub[160];
    if (find_chub(chub, sizeof chub) == 0 && write_file(chub, "1") < 0) {
        if (!hub_failed) logmsg("sensors: %s: %s", chub, strerror(errno));
        hub_failed = 1;
        return -1;
    }
    for (int i = 0; i < 40 && find_iio("accelerometer_sensor", acc_dev, sizeof acc_dev) < 0; i++) usleep(100000);
    if (!acc_dev[0]) {
        if (!hub_failed) logmsg("sensors: no accelerometer_sensor IIO device (no SSP sensor hub?)");
        hub_failed = 1;
        return -1;
    }
    find_iio("device_orientation", ori_dev, sizeof ori_dev);
    powered = 1;
    logmsg("sensors: hub up (accel %s, orientation %s)", acc_dev, ori_dev[0] ? ori_dev : "-");
    return 0;
}

void sensors_set_running(int on) {
    if (on == running) return;
    if (on && hub_up() < 0) return;
    if (on) {
        if (ori_dev[0]) { ssp_enable(T_ORIENT, 1); ori_fd = open_stream(ori_dev); }
        ssp_enable(T_ACCEL, 1);
        acc_fd = open_stream(acc_dev);
        candidate = -1;
    } else {
        if (acc_fd >= 0) { close(acc_fd); acc_fd = -1; }
        if (ori_fd >= 0) { close(ori_fd); ori_fd = -1; }
        ssp_enable(T_ACCEL, 0);
        if (ori_dev[0]) ssp_enable(T_ORIENT, 0);
    }
    running = on;
    logmsg("sensors: autorotate %s", on ? "running" : "stopped");
}

int sensors_fd(int which) { return which ? ori_fd : acc_fd; }

/* Orientation from gravity in the panel frame (x right, y up toward the top edge, z out of
 * the screen, positive when the axis points up). Returns 0/90/180/270 or -1 when unclear
 * (flat on a table, or near 45 degrees). */
static int orientation_of(int x, int y, int z) {
    int ax = abs(x), ay = abs(y), az = abs(z);
    if (az > G_UNITS * 85 / 100) return -1;                   /* lying flat */
    if (ay > G_UNITS * 70 / 100 && ay * 2 > ax * 3) return y > 0 ? 0 : 180;
    if (ax > G_UNITS * 70 / 100 && ax * 2 > ay * 3) return x > 0 ? 90 : 270;
    return -1;
}

static void propose(int r) {
    if (r < 0) { candidate = -1; return; }
    if (r != candidate) { candidate = r; cand_since = now_ms(); return; }
    if (r != current && now_ms() - cand_since >= STABLE_MS) {
        current = r;
        logmsg("sensors: orientation %d", r);
        if (store_get_int("rotation", -1) < 0) hm_set_rotation(r);
    }
}

/* Called by the main loop when sensors_fd(which) is readable. */
void sensors_read(int which) {
    uint8_t b[14 * 32];
    ssize_t n;
    if (which == 0 && acc_fd >= 0) {
        while ((n = read(acc_fd, b, sizeof b)) >= 14)
            for (ssize_t o = 0; o + 14 <= n; o += 14) {
                int16_t x, y, z;
                memcpy(&x, b + o, 2); memcpy(&y, b + o + 2, 2); memcpy(&z, b + o + 4, 2);
                acc_samples++;
                if (!ori_events) propose(orientation_of(x, y, z)); /* fallback until the hub reports */
            }
    } else if (which == 1 && ori_fd >= 0) {
        while ((n = read(ori_fd, b, sizeof b)) >= 9)
            for (ssize_t o = 0; o + 9 <= n; o += 9) {
                ori_events++;
                /* Android device orientation: 0 upright, 1 rotated left (90), 2 upside down, 3 right */
                int r = b[o] <= 3 ? b[o] * 90 : -1;
                logmsg("sensors: hub orientation event %d", b[o]);
                if (r >= 0) { candidate = r; cand_since = 0; propose(r); }
                if (ori_events == 1 && acc_fd >= 0) { /* the hub works: stop the accelerometer stream */
                    close(acc_fd); acc_fd = -1; ssp_enable(T_ACCEL, 0);
                    logmsg("sensors: using the hub's orientation, accelerometer off");
                }
            }
    }
}

int sensors_stats(char *buf, size_t len) {
    return snprintf(buf, len, "powered %d\nrunning %d\naccel_samples %lu\norientation_events %lu\ncurrent %d\ncandidate %d\n",
                    powered, running, acc_samples, ori_events, current, candidate);
}
