// Persistent settings in the unused tail of phonepanel's own partition.
//
// Region: [partition_end - 2 MiB, partition_end - 1 MiB), two 64 KiB slots. Each slot
// holds "PPST", sequence, length, CRC32, then "key=value\n" text. The newest valid
// slot wins; saves go to the other slot, so a power cut mid-write never loses the
// previous settings. The image builder keeps 2 MiB headroom and scripts/phone.sh
// skips this region when flashing, so deploys don't wipe the settings.
#include "paneld.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define SLOT_SIZE (64 * 1024)
#define REGION_FROM_END (2 * 1024 * 1024)
#define MAGIC 0x54535050u /* "PPST" little-endian */
#define MAX_KEYS 128

static char blkdev[64];
static long long region_off = -1;
static unsigned cur_seq;
static int cur_slot = -1;
static struct { char k[48]; char v[208]; } kv[MAX_KEYS];
static int nkv;

static uint32_t crc32(const void *buf, size_t n) {
    const uint8_t *p = buf;
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

/* Finds the block device whose PARTNAME is `part` (no /dev/block/by-name without ueventd). */
static int find_partition(const char *part, char *dev, size_t devlen, long long *bytes) {
    DIR *d = opendir("/sys/class/block");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found < 0) {
        if (e->d_name[0] == '.') continue;
        char p[300], buf[512];
        snprintf(p, sizeof p, "/sys/class/block/%s/uevent", e->d_name);
        if (read_file(p, buf, sizeof buf) <= 0) continue;
        char want[80];
        snprintf(want, sizeof want, "PARTNAME=%s\n", part);
        strcat(buf, "\n");
        if (!strstr(buf, want)) continue;
        snprintf(p, sizeof p, "/sys/class/block/%s/size", e->d_name);
        if (read_file(p, buf, sizeof buf) <= 0) continue;
        *bytes = atoll(buf) * 512;
        snprintf(dev, devlen, "/dev/%.32s", e->d_name);
        if (access(dev, F_OK) != 0) { /* devtmpfs normally has it; otherwise make the block node */
            snprintf(p, sizeof p, "/sys/class/block/%s/dev", e->d_name);
            unsigned ma, mi;
            if (read_file(p, buf, sizeof buf) > 0 && sscanf(buf, "%u:%u", &ma, &mi) == 2)
                mknod(dev, S_IFBLK | 0600, makedev(ma, mi));
        }
        found = 0;
    }
    closedir(d);
    return found;
}

static void parse(const char *txt, size_t len) {
    nkv = 0;
    const char *p = txt, *end = txt + len;
    while (p < end && nkv < MAX_KEYS) {
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) nl = end;
        const char *eq = memchr(p, '=', nl - p);
        if (eq && eq > p) {
            snprintf(kv[nkv].k, sizeof kv[nkv].k, "%.*s", (int)(eq - p), p);
            snprintf(kv[nkv].v, sizeof kv[nkv].v, "%.*s", (int)(nl - eq - 1), eq + 1);
            nkv++;
        }
        p = nl + 1;
    }
}

int store_init(const char *partition) {
    long long size;
    if (find_partition(partition, blkdev, sizeof blkdev, &size) < 0) {
        logmsg("store: partition '%s' not found; settings won't persist", partition);
        return -1;
    }
    region_off = size - REGION_FROM_END;
    int fd = open(blkdev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { logmsg("store: open %s: %s", blkdev, strerror(errno)); region_off = -1; return -1; }
    static uint8_t slot[SLOT_SIZE];
    for (int s = 0; s < 2; s++) {
        if (pread(fd, slot, SLOT_SIZE, region_off + (long long)s * SLOT_SIZE) != SLOT_SIZE) continue;
        uint32_t magic, seq, len, crc;
        memcpy(&magic, slot, 4); memcpy(&seq, slot + 4, 4); memcpy(&len, slot + 8, 4); memcpy(&crc, slot + 12, 4);
        if (magic != MAGIC || len > SLOT_SIZE - 16 || crc32(slot + 16, len) != crc) continue;
        if (cur_slot < 0 || seq > cur_seq) {
            cur_slot = s; cur_seq = seq;
            parse((const char *)slot + 16, len);
        }
    }
    close(fd);
    logmsg("store: %s @%lld: %s (%d keys, seq %u)", blkdev, region_off,
           cur_slot < 0 ? "empty" : "loaded", nkv, cur_seq);
    return 0;
}

static int safe_mode;
void store_set_safe_mode(int on) { safe_mode = on; }
int store_safe_mode(void) { return safe_mode; }

/* In safe mode (Vol Down held at boot) every saved setting reads as its default for this boot; nothing is erased,
 * and settings changed now are saved as usual. */
const char *store_get(const char *key, const char *dflt) {
    if (safe_mode) {
        for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key) && !strcmp(key, "last_reboot")) return kv[i].v;
        return dflt;
    }
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].k, key)) return kv[i].v;
    return dflt;
}

int store_get_int(const char *key, int dflt) {
    const char *v = store_get(key, NULL);
    return v && *v ? atoi(v) : dflt;
}

int store_set(const char *key, const char *value) {
    int i;
    for (i = 0; i < nkv && strcmp(kv[i].k, key); i++) {}
    if (i == nkv) {
        if (nkv == MAX_KEYS) return -1;
        nkv++;
    }
    snprintf(kv[i].k, sizeof kv[i].k, "%s", key);
    snprintf(kv[i].v, sizeof kv[i].v, "%s", value);
    return store_save();
}

int store_save(void) {
    if (region_off < 0) return -1;
    static uint8_t slot[SLOT_SIZE];
    memset(slot, 0, sizeof slot);
    size_t len = 0;
    for (int i = 0; i < nkv; i++)
        len += snprintf((char *)slot + 16 + len, SLOT_SIZE - 16 - len, "%s=%s\n", kv[i].k, kv[i].v);
    uint32_t magic = MAGIC, seq = cur_seq + 1, l = (uint32_t)len, crc = crc32(slot + 16, len);
    memcpy(slot, &magic, 4); memcpy(slot + 4, &seq, 4); memcpy(slot + 8, &l, 4); memcpy(slot + 12, &crc, 4);
    int target = cur_slot == 0 ? 1 : 0;
    int fd = open(blkdev, O_WRONLY | O_CLOEXEC);
    if (fd < 0) { logmsg("store: open for write: %s", strerror(errno)); return -1; }
    ssize_t w = pwrite(fd, slot, SLOT_SIZE, region_off + (long long)target * SLOT_SIZE);
    int rc = (w == SLOT_SIZE && fsync(fd) == 0) ? 0 : -1;
    close(fd);
    if (rc == 0) { cur_slot = target; cur_seq = seq; }
    else logmsg("store: write failed: %s", strerror(errno));
    return rc;
}

int store_dump(char *buf, size_t len) {
    int n = snprintf(buf, len, "# %s slot %d seq %u\n", blkdev[0] ? blkdev : "(none)", cur_slot, cur_seq);
    for (int i = 0; i < nkv && n < (int)len; i++) {
        size_t kl = strlen(kv[i].k);
        int secret = (kl > 4 && !strcmp(kv[i].k + kl - 4, ".psk")) || (kl > 6 && !strcmp(kv[i].k + kl - 6, ".token")) || !strcmp(kv[i].k, "bt.keys"); /* never echo secrets (incl. Bluetooth link keys) */
        n += snprintf(buf + n, len - n, "%s=%s\n", kv[i].k, secret && kv[i].v[0] ? "********" : kv[i].v);
    }
    return n < (int)len ? n : (int)len - 1;
}
