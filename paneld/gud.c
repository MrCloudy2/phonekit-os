// GUD (Generic USB Display) device over FunctionFS: the phone becomes a USB monitor.
//
// The host side is Linux's mainline gud driver (drivers/gpu/drm/gud, since 5.13). It binds to
// a vendor-class interface on VID:PID 1d50:614d, so mode A uses that USB ID.
// Protocol: include/drm/gud.h. Control requests (vendor, interface recipient) configure the
// display; each flush is a SET_BUFFER (rectangle + length) followed by a bulk OUT transfer of
// the pixels. We advertise STATUS_ON_SET, so the host reads GET_STATUS after every SET.
//
// Threads: ep0 (control) and bulk run separately: the host asks for status right after
// SET_BUFFER, before it sends the (megabyte-sized) bulk transfer.
#include "paneld.h"

#include <endian.h>
#include <sched.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define FFS_DIR "/dev/usb-ffs/gud"

/* ---- protocol (subset of include/drm/gud.h, MIT) ---- */
#define GUD_DISPLAY_MAGIC 0x1d50614d
#define GUD_DISPLAY_FLAG_STATUS_ON_SET (1u << 0)
#define GUD_COMPRESSION_LZ4 (1u << 0)
#define GUD_REQ_GET_STATUS 0x00
#define GUD_REQ_GET_DESCRIPTOR 0x01
#define GUD_REQ_GET_FORMATS 0x40
#define GUD_REQ_GET_PROPERTIES 0x41
#define GUD_REQ_GET_CONNECTORS 0x50
#define GUD_REQ_GET_CONNECTOR_PROPERTIES 0x51
#define GUD_REQ_SET_CONNECTOR_FORCE_DETECT 0x53
#define GUD_REQ_GET_CONNECTOR_STATUS 0x54
#define GUD_REQ_GET_CONNECTOR_MODES 0x55
#define GUD_REQ_GET_CONNECTOR_EDID 0x56
#define GUD_REQ_SET_BUFFER 0x60
#define GUD_REQ_SET_STATE_CHECK 0x61
#define GUD_REQ_SET_STATE_COMMIT 0x62
#define GUD_REQ_SET_CONTROLLER_ENABLE 0x63
#define GUD_REQ_SET_DISPLAY_ENABLE 0x64
#define GUD_STATUS_OK 0x00
#define GUD_STATUS_REQUEST_NOT_SUPPORTED 0x02
#define GUD_STATUS_PROTOCOL_ERROR 0x03
#define GUD_STATUS_INVALID_PARAMETER 0x04
#define GUD_PIXEL_FORMAT_RGB565 0x40
#define GUD_PIXEL_FORMAT_XRGB8888 0x80
#define GUD_CONNECTOR_TYPE_PANEL 0
#define GUD_CONNECTOR_STATUS_CONNECTED 0x01
#define GUD_DISPLAY_MODE_FLAG_PREFERRED (1u << 10)
#define GUD_PROPERTY_BACKLIGHT_BRIGHTNESS 12
#define GUD_PROPERTY_ROTATION 50
#define GUD_ROTATION_0 (1u << 0)

struct __attribute__((packed)) gud_display_descriptor_req {
    uint32_t magic; uint8_t version; uint32_t flags; uint8_t compression; uint32_t max_buffer_size;
    uint32_t min_width, max_width, min_height, max_height;
};
struct __attribute__((packed)) gud_property_req { uint16_t prop; uint64_t val; };
struct __attribute__((packed)) gud_display_mode_req {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, vdisplay, vsync_start, vsync_end, vtotal;
    uint32_t flags;
};
struct __attribute__((packed)) gud_connector_descriptor_req { uint8_t connector_type; uint32_t flags; };
struct __attribute__((packed)) gud_set_buffer_req {
    uint32_t x, y, width, height, length; uint8_t compression; uint32_t compressed_length;
};
struct __attribute__((packed)) gud_state_req {
    struct gud_display_mode_req mode; uint8_t format; uint8_t connector;
    struct gud_property_req properties[];
};

/* ---- state ---- */
static struct fb *fb;
static int ep0 = -1, ep1 = -1;
static volatile int ready;              /* descriptors written: the function can be bound */
static volatile int enabled;            /* host enabled the USB function (config set) */
static volatile int active;             /* host owns the screen (display enabled + committed) */
static uint8_t last_status = GUD_STATUS_OK;
static uint8_t cur_format = GUD_PIXEL_FORMAT_RGB565, pending_format = GUD_PIXEL_FORMAT_RGB565;
static int use_lz4 = 1, want_xrgb;
static int pending_backlight = -1;
static unsigned long frames, bytes_in, px_in, pans;
static long t_usb_us, t_lz4_us, t_blit_us, t_flush_us; /* summed per phase */
static long last_frame_ms, first_frame_ms;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int buf_pending; /* a SET_BUFFER waits for its bulk data */
static struct gud_set_buffer_req pend;

int gud_ready(void) { return ready; }
int gud_active(void) { return active && enabled; }

int gud_stats(char *b, size_t len) {
    long span = last_frame_ms - first_frame_ms;
    long f = frames ? (long)frames : 1;
    return snprintf(b, len, "ready %d\nenabled %d\nactive %d\nformat 0x%02x\nlz4 %d\nframes %lu\nbytes %lu\nraw_bytes %lu\nfps_avg %.1f\n"
                    "ms_per_frame usb %.2f lz4 %.2f blit %.2f flush %.2f\npans %lu\n",
                    ready, enabled, active, cur_format, use_lz4, frames, bytes_in, px_in,
                    span > 0 && frames > 1 ? (frames - 1) * 1000.0 / span : 0.0,
                    t_usb_us / 1000.0 / f, t_lz4_us / 1000.0 / f, t_blit_us / 1000.0 / f, t_flush_us / 1000.0 / f, pans);
}

void gud_stats_reset(void) { pans = 0; frames = bytes_in = px_in = 0; t_usb_us = t_lz4_us = t_blit_us = t_flush_us = 0; first_frame_ms = last_frame_ms = 0; }

/* ---- FunctionFS descriptors ---- */
static int write_descriptors(void) {
    struct __attribute__((packed)) {
        struct usb_functionfs_descs_head_v2 header;
        uint32_t fs_count, hs_count;
        struct __attribute__((packed)) {
            struct usb_interface_descriptor intf;
            struct usb_endpoint_descriptor_no_audio out;
        } fs, hs;
    } d;
    memset(&d, 0, sizeof d);
    d.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.header.length = htole32(sizeof d);
    d.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.fs_count = d.hs_count = htole32(2);
    struct usb_interface_descriptor intf = {
        .bLength = sizeof intf, .bDescriptorType = USB_DT_INTERFACE, .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC, .iInterface = 1};
    d.fs.intf = d.hs.intf = intf;
    struct usb_endpoint_descriptor_no_audio out = {
        .bLength = sizeof out, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 1 | USB_DIR_OUT, .bmAttributes = USB_ENDPOINT_XFER_BULK};
    d.fs.out = d.hs.out = out;
    d.fs.out.wMaxPacketSize = htole16(64);
    d.hs.out.wMaxPacketSize = htole16(512);

    struct __attribute__((packed)) {
        struct usb_functionfs_strings_head header;
        struct __attribute__((packed)) { uint16_t code; char str1[sizeof "phonepanel display"]; } lang0;
    } s = {
        .header = {.magic = htole32(FUNCTIONFS_STRINGS_MAGIC), .length = htole32(sizeof s),
                   .str_count = htole32(1), .lang_count = htole32(1)},
        .lang0 = {htole16(0x0409), "phonepanel display"},
    };
    if (write(ep0, &d, sizeof d) != sizeof d) { logmsg("gud: descriptors: %s", strerror(errno)); return -1; }
    if (write(ep0, &s, sizeof s) != sizeof s) { logmsg("gud: strings: %s", strerror(errno)); return -1; }
    return 0;
}

/* ---- LZ4 block decompression (the host compresses with the kernel's LZ4) ---- */
__attribute__((optimize("O3"))) static long lz4_decompress(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen) {
    const uint8_t *ip = src, *iend = src + slen;
    uint8_t *op = dst, *oend = dst + dlen;
    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return -1;
        memcpy(op, ip, lit);
        ip += lit; op += lit;
        if (ip >= iend) break; /* the last sequence has literals only */
        if (iend - ip < 2) return -1;
        size_t off = ip[0] | (size_t)ip[1] << 8;
        ip += 2;
        if (!off || off > (size_t)(op - dst)) return -1;
        size_t ml = token & 15;
        if (ml == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255);
        }
        ml += 4;
        if ((size_t)(oend - op) < ml) return -1;
        const uint8_t *m = op - off;
        if (off >= ml) memcpy(op, m, ml);
        else if (off == 1) memset(op, *m, ml); /* runs of one byte, common in UI content */
        else for (size_t i = 0; i < ml; i++) op[i] = m[i];
        op += ml;
    }
    return op - dst;
}

/* ---- visibility: shadow copy while paneld owns the screen ---- */
static uint8_t *shadow;          /* same layout as the framebuffer */
static volatile int visible = 1;

void gud_set_visible(int v) {
    if (!fb || !fb->mem || v == visible) return;
    size_t len = (size_t)fb->ph * fb->stride;
    if (!shadow) shadow = malloc(len);
    if (!shadow) return;
    if (!v) { memcpy(shadow, fb->mem, len); visible = 0; }  /* keep the host's picture */
    else { visible = 1; memcpy(fb->mem, shadow, len); if (active) fb_flush(fb); }
}

static uint8_t *target(void) { return visible || !shadow ? fb->mem : shadow; }

/* ---- blit ---- */
__attribute__((optimize("O3"))) static void blit(const struct gud_set_buffer_req *r, const uint8_t *src) {
    if (!fb || !fb->mem) return;
    int bypp = cur_format == GUD_PIXEL_FORMAT_RGB565 ? 2 : 4;
    for (uint32_t row = 0; row < r->height; row++) {
        uint32_t y = r->y + row;
        if ((int)y >= fb->ph) break;
        uint32_t *dst = (uint32_t *)(target() + (size_t)y * fb->stride) + r->x;
        const uint8_t *s = src + (size_t)row * r->width * bypp;
        uint32_t w = r->width;
        if ((int)(r->x + w) > fb->pw) w = fb->pw - r->x;
        const int rs = fb->var.red.offset, bs = fb->var.blue.offset; /* true panel layout */
        if (bypp == 4) { /* host XRGB8888 (R at 16) -> panel; force alpha opaque */
            const uint32_t *p = (const uint32_t *)s;
            if (rs == 16)
                for (uint32_t x = 0; x < w; x++) dst[x] = p[x] | 0xff000000u;
            else /* panel is BGR: swap R and B */
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = p[x];
                    dst[x] = 0xff000000u | (v & 0x00ff00u) | (v >> 16 & 0xff) | (v & 0xff) << 16;
                }
        } else {         /* RGB565 -> panel 8888 */
            const uint16_t *p = (const uint16_t *)s;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = p[x], r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
                dst[x] = 0xff000000u | (r5 << 3 | r5 >> 2) << rs | (g6 << 2 | g6 >> 4) << 8 | (b5 << 3 | b5 >> 2) << bs;
            }
        }
    }
}

static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fcv = PTHREAD_COND_INITIALIZER;
static int flush_wanted;

/* Pans (which waits for vsync) off the bulk path. Video-mode DECON scans the buffer
 * continuously, so blitting while a pan is pending only risks the tearing a video-mode
 * panel has anyway; requests that pile up during one vsync coalesce into one pan. */
static void *flush_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&fmu);
        while (!flush_wanted) pthread_cond_wait(&fcv, &fmu);
        flush_wanted = 0;
        pthread_mutex_unlock(&fmu);
        if (active && fb && !fb->blanked) { fb_flush(fb); pans++; } /* never pan a powered-down panel */
    }
    return NULL;
}

static void request_flush(void) {
    pthread_mutex_lock(&fmu);
    flush_wanted = 1;
    pthread_cond_signal(&fcv);
    pthread_mutex_unlock(&fmu);
}

/* ---- receive -> render pipeline ----
 * bulk thread: receives frame N+1 into one buffer while the render thread decompresses
 * and converts frame N from the other. The conversion is split across BLIT_WORKERS
 * threads. All of them run on the fast cluster. */
#define BLIT_WORKERS 3   /* max: render thread + 2 helpers */
static int blit_workers = 1; /* used; `gud workers N` (store gud_workers) */

struct frame { struct gud_set_buffer_req r; uint8_t *buf; size_t cap, len; };
static struct frame slots[2];
static int render_slot = -1;           /* slot handed to the render thread, -1 = none */
static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rcv = PTHREAD_COND_INITIALIZER;

/* parallel blit */
static struct { const struct gud_set_buffer_req *r; const uint8_t *src; } job;
static unsigned job_gen, job_done;
static pthread_mutex_t bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv = PTHREAD_COND_INITIALIZER, bdone = PTHREAD_COND_INITIALIZER;

static void blit_part(int part) {
    struct gud_set_buffer_req r = *job.r;
    uint32_t h0 = r.height * part / blit_workers, h1 = r.height * (part + 1) / blit_workers;
    int bypp = cur_format == GUD_PIXEL_FORMAT_RGB565 ? 2 : 4;
    const uint8_t *src = job.src + (size_t)h0 * r.width * bypp;
    r.y += h0;
    r.height = h1 - h0;
    if (r.height) blit(&r, src);
}

static void *blit_worker(void *arg) {
    int part = (int)(long)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&bmu);
        while (job_gen == seen) pthread_cond_wait(&bcv, &bmu);
        seen = job_gen;
        pthread_mutex_unlock(&bmu);
        if (part < blit_workers) blit_part(part);
        pthread_mutex_lock(&bmu);
        job_done++;
        pthread_cond_signal(&bdone);
        pthread_mutex_unlock(&bmu);
    }
    return NULL;
}

static void blit_parallel(const struct gud_set_buffer_req *r, const uint8_t *src) {
    if (r->height < 64 || blit_workers <= 1) { blit(r, src); return; } /* small damage: not worth waking helpers */
    pthread_mutex_lock(&bmu);
    job.r = r; job.src = src; job_done = 0; job_gen++;
    pthread_cond_broadcast(&bcv);
    pthread_mutex_unlock(&bmu);
    blit_part(0);
    pthread_mutex_lock(&bmu);
    while (job_done < BLIT_WORKERS - 1) pthread_cond_wait(&bdone, &bmu);
    pthread_mutex_unlock(&bmu);
}

static void *render_thread(void *arg) {
    (void)arg;
    uint8_t *raw = NULL;
    size_t rawcap = 0;
    for (;;) {
        pthread_mutex_lock(&rmu);
        while (render_slot < 0) pthread_cond_wait(&rcv, &rmu);
        struct frame *f = &slots[render_slot];
        pthread_mutex_unlock(&rmu);
        long t1 = now_us();
        const uint8_t *pix = f->buf;
        int okf = 1;
        if (f->r.compression) {
            if (f->r.length > rawcap) { free(raw); raw = malloc(f->r.length); rawcap = raw ? f->r.length : 0; }
            long out = raw ? lz4_decompress(f->buf, f->len, raw, f->r.length) : -1;
            if (out != (long)f->r.length) { logmsg("gud: lz4 frame corrupt (%ld of %u)", out, f->r.length); okf = 0; }
            pix = raw;
        }
        long t2 = now_us();
        if (okf) {
            blit_parallel(&f->r, pix);
            long t3 = now_us();
            if (visible) request_flush();
            t_lz4_us += t2 - t1; t_blit_us += t3 - t2;
            frames++;
            bytes_in += f->len;
            px_in += f->r.length;
            last_frame_ms = now_ms();
            if (!first_frame_ms) first_frame_ms = last_frame_ms;
        }
        pthread_mutex_lock(&rmu);
        render_slot = -1; /* slot free again */
        pthread_cond_broadcast(&rcv);
        pthread_mutex_unlock(&rmu);
    }
    return NULL;
}

static void *bulk_thread(void *arg) {
    (void)arg;
    int cur = 0;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!buf_pending) pthread_cond_wait(&cv, &mu);
        struct gud_set_buffer_req r = pend;
        pthread_mutex_unlock(&mu);
        if (ep1 < 0) ep1 = open(FFS_DIR "/ep1", O_RDONLY | O_CLOEXEC);
        struct frame *f = &slots[cur];
        size_t len = r.compression ? r.compressed_length : r.length;
        if (len > f->cap) { free(f->buf); f->buf = malloc(len); f->cap = f->buf ? len : 0; }
        size_t got = 0;
        long t0 = now_us();
        while (f->buf && ep1 >= 0 && got < len) { /* the transfer may arrive in several reads */
            ssize_t n = read(ep1, f->buf + got, len - got > (1u << 20) ? (1u << 20) : len - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { logmsg("gud: bulk read: %s", n < 0 ? strerror(errno) : "EOF"); close(ep1); ep1 = -1; break; }
            got += n;
        }
        t_usb_us += now_us() - t0;
        /* the host may send the next SET_BUFFER as soon as the bulk data is in */
        pthread_mutex_lock(&mu);
        buf_pending = 0;
        pthread_mutex_unlock(&mu);
        if (got != len) continue;
        f->r = r;
        f->len = len;
        /* hand over once the renderer is done with the previous frame (it owns the other slot) */
        pthread_mutex_lock(&rmu);
        while (render_slot >= 0) pthread_cond_wait(&rcv, &rmu);
        render_slot = cur;
        pthread_cond_broadcast(&rcv);
        pthread_mutex_unlock(&rmu);
        cur ^= 1;
    }
    return NULL;
}

/* Pin the calling thread to the fastest cluster (highest cpuinfo_max_freq). */
static void pin_to_big_cores(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    long best = 0;
    for (int c = 0; c < 16; c++) {
        char p[96], v[32];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        if (read_file(p, v, sizeof v) <= 0) continue;
        long f = atol(v);
        if (f > best) { best = f; CPU_ZERO(&set); }
        if (f == best) CPU_SET(c, &set);
    }
    if (best) pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static void *start_pinned(void *arg) {
    void *(*fn)(void *) = ((void *(**)(void *))arg)[0];
    void *a = ((void **)arg)[1];
    free(arg);
    pin_to_big_cores();
    return fn(a);
}

static void spawn_pinned(void *(*fn)(void *), void *a) {
    void **pa = malloc(2 * sizeof *pa);
    pa[0] = (void *)fn; pa[1] = a;
    pthread_t t;
    pthread_create(&t, NULL, start_pinned, pa);
    pthread_detach(t);
}

/* ---- control requests ---- */
static void reply(const void *data, size_t len, size_t wlen) {
    if (len > wlen) len = wlen;
    if (write(ep0, data, len) < 0) logmsg("gud: ep0 write: %s", strerror(errno));
}

static void stall_in(void) { (void)!read(ep0, NULL, 0); }   /* opposite direction = stall */

static void mode_of(struct gud_display_mode_req *m) {
    int w = fb ? fb->pw : 1080, h = fb ? fb->ph : 2340; /* panel-native mode */
    memset(m, 0, sizeof *m);
    m->hdisplay = w; m->hsync_start = w + 1; m->hsync_end = w + 2; m->htotal = w + 3;
    m->vdisplay = h; m->vsync_start = h + 1; m->vsync_end = h + 2; m->vtotal = h + 3;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
    m->flags = GUD_DISPLAY_MODE_FLAG_PREFERRED;
}

static void handle_setup(const struct usb_ctrlrequest *s) {
    uint8_t req = s->bRequest;
    uint16_t wlen = le16toh(s->wLength);
    int in = s->bRequestType & USB_DIR_IN;
    if ((s->bRequestType & USB_TYPE_MASK) != USB_TYPE_VENDOR) { in ? stall_in() : (void)!write(ep0, NULL, 0); return; }
    if (in) {
        uint8_t b[256];
        size_t n = 0;
        switch (req) {
        case GUD_REQ_GET_STATUS: b[0] = last_status; n = 1; break;
        case GUD_REQ_GET_DESCRIPTOR: {
            struct gud_display_descriptor_req d = {
                .magic = htole32(GUD_DISPLAY_MAGIC), .version = 1, .flags = htole32(GUD_DISPLAY_FLAG_STATUS_ON_SET),
                .compression = use_lz4 ? GUD_COMPRESSION_LZ4 : 0, .max_buffer_size = htole32((uint32_t)fb->pw * fb->ph * 4),
                .min_width = htole32(fb->pw), .max_width = htole32(fb->pw),
                .min_height = htole32(fb->ph), .max_height = htole32(fb->ph)};
            memcpy(b, &d, sizeof d); n = sizeof d;
            break;
        }
        case GUD_REQ_GET_FORMATS: /* only what we want on the wire; the host converts XRGB itself */
            b[0] = want_xrgb ? GUD_PIXEL_FORMAT_XRGB8888 : GUD_PIXEL_FORMAT_RGB565;
            n = 1;
            break;
        case GUD_REQ_GET_PROPERTIES: {
            struct gud_property_req p = {htole16(GUD_PROPERTY_ROTATION), htole64(GUD_ROTATION_0)};
            memcpy(b, &p, sizeof p); n = sizeof p;
            break;
        }
        case GUD_REQ_GET_CONNECTORS: {
            struct gud_connector_descriptor_req c = {GUD_CONNECTOR_TYPE_PANEL, 0};
            memcpy(b, &c, sizeof c); n = sizeof c;
            break;
        }
        case GUD_REQ_GET_CONNECTOR_PROPERTIES: {
            struct gud_property_req p = {htole16(GUD_PROPERTY_BACKLIGHT_BRIGHTNESS), htole64(50)};
            memcpy(b, &p, sizeof p); n = sizeof p;
            break;
        }
        case GUD_REQ_GET_CONNECTOR_STATUS: b[0] = GUD_CONNECTOR_STATUS_CONNECTED; n = 1; break;
        case GUD_REQ_GET_CONNECTOR_MODES: {
            struct gud_display_mode_req m;
            mode_of(&m);
            memcpy(b, &m, sizeof m); n = sizeof m;
            break;
        }
        case GUD_REQ_GET_CONNECTOR_EDID: n = 0; break; /* modes above are enough */
        default:
            last_status = GUD_STATUS_REQUEST_NOT_SUPPORTED;
            logmsg("gud: unsupported IN request 0x%02x", req);
            stall_in();
            return;
        }
        if (req != GUD_REQ_GET_STATUS) last_status = GUD_STATUS_OK;
        reply(b, n, wlen);
        return;
    }
    /* OUT: read the payload (also completes the status stage) */
    uint8_t b[512];
    ssize_t n = 0;
    if (wlen) {
        n = read(ep0, b, wlen < sizeof b ? wlen : sizeof b);
        if (n < 0) { logmsg("gud: ep0 read: %s", strerror(errno)); return; }
    } else (void)!read(ep0, NULL, 0);
    last_status = GUD_STATUS_OK;
    switch (req) {
    case GUD_REQ_SET_CONNECTOR_FORCE_DETECT: break;
    case GUD_REQ_SET_BUFFER: {
        if (n < (ssize_t)sizeof(struct gud_set_buffer_req)) { last_status = GUD_STATUS_PROTOCOL_ERROR; break; }
        struct gud_set_buffer_req r;
        memcpy(&r, b, sizeof r);
        r.x = le32toh(r.x); r.y = le32toh(r.y); r.width = le32toh(r.width); r.height = le32toh(r.height);
        r.length = le32toh(r.length); r.compressed_length = le32toh(r.compressed_length);
        if ((r.compression & ~GUD_COMPRESSION_LZ4) || (int)(r.x + r.width) > fb->pw || (int)(r.y + r.height) > fb->ph ||
            r.length > (uint32_t)fb->pw * fb->ph * 4) { last_status = GUD_STATUS_INVALID_PARAMETER; break; }
        pthread_mutex_lock(&mu);
        for (int i = 0; i < 2000 && buf_pending; i++) { /* previous transfer still landing */
            pthread_mutex_unlock(&mu); usleep(1000); pthread_mutex_lock(&mu);
        }
        pend = r;
        buf_pending = 1;
        pthread_cond_signal(&cv);
        pthread_mutex_unlock(&mu);
        break;
    }
    case GUD_REQ_SET_STATE_CHECK: {
        if (n < (ssize_t)sizeof(struct gud_state_req)) { last_status = GUD_STATUS_PROTOCOL_ERROR; break; }
        struct gud_state_req *st = (struct gud_state_req *)b;
        if (st->format != GUD_PIXEL_FORMAT_XRGB8888 && st->format != GUD_PIXEL_FORMAT_RGB565) {
            last_status = GUD_STATUS_INVALID_PARAMETER;
            break;
        }
        if (le16toh(st->mode.hdisplay) != fb->pw || le16toh(st->mode.vdisplay) != fb->ph) {
            last_status = GUD_STATUS_INVALID_PARAMETER;
            break;
        }
        pending_format = st->format;
        size_t np = (n - sizeof *st) / sizeof(struct gud_property_req);
        for (size_t i = 0; i < np; i++) {
            struct gud_property_req p;
            memcpy(&p, (uint8_t *)st->properties + i * sizeof p, sizeof p);
            if (le16toh(p.prop) == GUD_PROPERTY_BACKLIGHT_BRIGHTNESS) pending_backlight = (int)le64toh(p.val);
        }
        break;
    }
    case GUD_REQ_SET_STATE_COMMIT:
        cur_format = pending_format;
        if (pending_backlight >= 0) power_set_percent(pending_backlight);
        active = 1;
        logmsg("gud: state committed: %dx%d %s", fb->pw, fb->ph, cur_format == GUD_PIXEL_FORMAT_RGB565 ? "RGB565" : "XRGB8888");
        break;
    case GUD_REQ_SET_CONTROLLER_ENABLE:
        if (n >= 1 && !b[0]) active = 0;
        logmsg("gud: controller %s", n >= 1 && b[0] ? "enabled" : "disabled");
        break;
    case GUD_REQ_SET_DISPLAY_ENABLE:
        if (n >= 1 && b[0]) power_wake("gud");
        else power_off("gud");
        logmsg("gud: display %s", n >= 1 && b[0] ? "on" : "off (DPMS)");
        break;
    default:
        last_status = GUD_STATUS_REQUEST_NOT_SUPPORTED;
        logmsg("gud: unsupported OUT request 0x%02x", req);
    }
}

/* FunctionFS holds its per-instance mutex while a blocking ep0 read() waits for events,
 * and functionfs_unbind() (UDC unbind during a mode switch) needs that mutex: a reader
 * parked in read() after the DISABLE event deadlocks the unbind (seen 2026-11-15, the
 * watchdog caught it in gadget_dev_desc_UDC_store -> functionfs_unbind). So: wait in
 * poll() (no mutex held), read events non-blocking, and only go blocking while
 * answering a setup request. */
static void ep0_blocking(int on) {
    int fl = fcntl(ep0, F_GETFL);
    fcntl(ep0, F_SETFL, on ? fl & ~O_NONBLOCK : fl | O_NONBLOCK);
}

static void *ep0_thread(void *arg) {
    (void)arg;
    struct usb_functionfs_event ev[4];
    ep0_blocking(0);
    for (;;) {
        struct pollfd pf = {.fd = ep0, .events = POLLIN};
        if (poll(&pf, 1, 1000) <= 0) continue;
        ssize_t n = read(ep0, ev, sizeof ev);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            logmsg("gud: ep0 events: %s", strerror(errno));
            sleep(1);
            continue;
        }
        for (int i = 0; i < (int)(n / (ssize_t)sizeof ev[0]); i++) {
            switch (ev[i].type) {
            case FUNCTIONFS_ENABLE: enabled = 1; logmsg("gud: function enabled by host"); break;
            case FUNCTIONFS_DISABLE:
            case FUNCTIONFS_UNBIND:
                if (enabled) logmsg("gud: function disabled");
                enabled = 0; active = 0;
                if (ep1 >= 0) { close(ep1); ep1 = -1; }
                break;
            case FUNCTIONFS_SETUP:
                ep0_blocking(1); /* data stage I/O must complete, not EAGAIN */
                handle_setup(&ev[i].u.setup);
                ep0_blocking(0);
                break;
            default: break;
            }
        }
    }
    return NULL;
}

/* Mount FunctionFS for the configfs function instance `ffs.gud`, write descriptors,
 * start the threads. Must run before the function is linked and the UDC bound. */
void gud_set_workers(int n) { blit_workers = n < 1 ? 1 : n > BLIT_WORKERS ? BLIT_WORKERS : n; }

int gud_init(struct fb *f, const char *configfs_gadget) {
    fb = f;
    gud_set_workers(store_get_int("gud_workers", 1));
    want_xrgb = !strcmp(store_get("gud_format", "rgb565"), "xrgb8888");
    use_lz4 = store_get_int("gud_lz4", 1);
    cur_format = pending_format = want_xrgb ? GUD_PIXEL_FORMAT_XRGB8888 : GUD_PIXEL_FORMAT_RGB565;
    if (!fb || !fb->mem) return -1;
    char p[160];
    snprintf(p, sizeof p, "%s/functions/ffs.gud", configfs_gadget);
    if (mkdir(p, 0755) < 0 && errno != EEXIST) { logmsg("gud: %s: %s", p, strerror(errno)); return -1; }
    mkdir("/dev/usb-ffs", 0755);
    mkdir(FFS_DIR, 0755);
    if (mount("gud", FFS_DIR, "functionfs", 0, NULL) < 0 && errno != EBUSY) {
        logmsg("gud: mount functionfs: %s", strerror(errno));
        return -1;
    }
    ep0 = open(FFS_DIR "/ep0", O_RDWR | O_CLOEXEC);
    if (ep0 < 0 || write_descriptors() < 0) { logmsg("gud: ep0: %s", strerror(errno)); return -1; }
    pthread_t t;
    pthread_create(&t, NULL, ep0_thread, NULL); pthread_detach(t);
    spawn_pinned(bulk_thread, NULL);
    spawn_pinned(render_thread, NULL);
    for (long i = 1; i < BLIT_WORKERS; i++) spawn_pinned(blit_worker, (void *)i);
    pthread_create(&t, NULL, flush_thread, NULL); pthread_detach(t);
    ready = 1;
    logmsg("gud: FunctionFS ready (%dx%d, %s%s)", fb->pw, fb->ph, want_xrgb ? "XRGB8888" : "RGB565", use_lz4 ? " + LZ4" : "");
    return 0;
}
