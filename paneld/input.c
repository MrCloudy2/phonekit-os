// evdev input discovery.
#include "paneld.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_LONG (sizeof(long) * 8)
#define TEST_BIT(b, a) (((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

/* Opens input devices not already in in[0..n); returns the new count. Call again
 * later to pick up devices that probe asynchronously (the touchscreen does). */
int inputs_scan(struct input *in, int n, int max) {
    DIR *d = opendir("/sys/class/input");
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "event", 5)) continue;
        int known = 0;
        for (int k = 0; k < n && !known; k++) known = !strcmp(in[k].node, e->d_name);
        static char ignored[32][16]; /* opened before, not interesting: don't log again */
        static int nignored;
        for (int k = 0; k < nignored && !known; k++) known = !strcmp(ignored[k], e->d_name);
        if (known) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        ensure_node("input", e->d_name, path);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input *i = &in[n];
        memset(i, 0, sizeof *i);
        i->fd = fd;
        snprintf(i->node, sizeof i->node, "%.15s", e->d_name);
        ioctl(fd, EVIOCGNAME(sizeof i->name), i->name);
        unsigned long keys[KEY_CNT / BITS_LONG + 1] = {0}, abs[ABS_CNT / BITS_LONG + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        i->has_power = TEST_BIT(KEY_POWER, keys);
        if (strcmp(i->name, "phonepanel-inject-keys")) keys_note_device(keys); /* not our own virtual keys */
        i->is_touch = TEST_BIT(ABS_MT_POSITION_X, abs);
        if (i->is_touch) {
            struct input_absinfo ax = {0}, ay = {0};
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
            i->absmax_x = ax.maximum; i->absmax_y = ay.maximum;
            touch_use_boottime(fd);
        }
        logmsg("input: %s '%s'%s%s", e->d_name, i->name, i->is_touch ? " [touch]" : "",
               i->has_power ? " [power]" : "");
        if (i->is_touch || i->has_power) n++;
        else {
            close(fd);
            if (nignored < 32) snprintf(ignored[nignored++], 16, "%.15s", e->d_name);
        }
    }
    closedir(d);
    return n;
}

int inputs_open(struct input *in, int max) { return inputs_scan(in, 0, max); }
