// fbdev display backend.
#include "paneld.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

uint32_t fb_color(const struct fb *f, uint8_t r, uint8_t g, uint8_t b) {
    const struct fb_var_screeninfo *v = &f->var;
    return ((uint32_t)(r >> (8 - v->red.length)) << v->red.offset) |
           ((uint32_t)(g >> (8 - v->green.length)) << v->green.offset) |
           ((uint32_t)(b >> (8 - v->blue.length)) << v->blue.offset) |
           (v->transp.length ? (((1u << v->transp.length) - 1) << v->transp.offset) : 0);
}

void fb_set_rotation(struct fb *f, int deg) {
    f->rot = ((deg % 360) + 360) % 360 / 90 * 90;
    f->w = (f->rot == 90 || f->rot == 270) ? f->ph : f->pw;
    f->h = (f->rot == 90 || f->rot == 270) ? f->pw : f->ph;
}

/* Adds a LOGICAL rect to the damage (panel coordinates). */
void fb_damage_rect(struct fb *f, int x0, int y0, int w, int h) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > f->w) w = f->w - x0;
    if (y0 + h > f->h) h = f->h - y0;
    if (w <= 0 || h <= 0) return;
    int px, py, pwid, phei;
    switch (f->rot) {
    case 90:  px = f->pw - (y0 + h); py = x0;               pwid = h; phei = w; break;
    case 180: px = f->pw - (x0 + w); py = f->ph - (y0 + h); pwid = w; phei = h; break;
    case 270: px = y0;               py = f->ph - (x0 + w); pwid = h; phei = w; break;
    default:  px = x0;               py = y0;               pwid = w; phei = h;
    }
    if (f->dx1 <= f->dx0) { f->dx0 = px; f->dy0 = py; f->dx1 = px + pwid; f->dy1 = py + phei; }
    else {
        if (px < f->dx0) f->dx0 = px;
        if (py < f->dy0) f->dy0 = py;
        if (px + pwid > f->dx1) f->dx1 = px + pwid;
        if (py + phei > f->dy1) f->dy1 = py + phei;
    }
}

/* (x0, y0, w, h) in LOGICAL coordinates; mapped onto the panel for the current rotation. */
void fb_fill(struct fb *f, int x0, int y0, int w, int h, uint32_t c) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > f->w) w = f->w - x0;
    if (y0 + h > f->h) h = f->h - y0;
    if (w <= 0 || h <= 0) return;
    int px, py, pwid, phei; /* same mapping as logical_to_panel() in touch.c */
    switch (f->rot) {
    case 90:  px = f->pw - (y0 + h); py = x0;               pwid = h; phei = w; break;
    case 180: px = f->pw - (x0 + w); py = f->ph - (y0 + h); pwid = w; phei = h; break;
    case 270: px = y0;               py = f->ph - (x0 + w); pwid = h; phei = w; break;
    default:  px = x0;               py = y0;               pwid = w; phei = h;
    }
    x0 = px; y0 = py; w = pwid; h = phei;
    /* grow the damage rectangle (panel coordinates) */
    if (f->dx1 <= f->dx0) { f->dx0 = x0; f->dy0 = y0; f->dx1 = x0 + w; f->dy1 = y0 + h; }
    else {
        if (x0 < f->dx0) f->dx0 = x0;
        if (y0 < f->dy0) f->dy0 = y0;
        if (x0 + w > f->dx1) f->dx1 = x0 + w;
        if (y0 + h > f->dy1) f->dy1 = y0 + h;
    }
    int bypp = f->bpp / 8;
    for (int y = y0; y < y0 + h; y++) {
        uint8_t *row = f->mem + (size_t)y * f->stride + (size_t)x0 * bypp;
        if (bypp == 4) {
            uint32_t *p = (uint32_t *)row;
            for (int x = 0; x < w; x++) p[x] = c;
        } else if (bypp == 2) {
            uint16_t *p = (uint16_t *)row;
            for (int x = 0; x < w; x++) p[x] = (uint16_t)c;
        } else {
            for (int x = 0; x < w; x++) memcpy(row + x * bypp, &c, bypp);
        }
    }
}

void fb_flush(struct fb *f) {
    /* DECON-style fbdev drivers update the panel on pan. */
    f->var.yoffset = 0;
    f->var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(f->fd, FBIOPAN_DISPLAY, &f->var) < 0)
        ioctl(f->fd, FBIOPUT_VSCREENINFO, &f->var);
}

void fb_damage_all(struct fb *f) { f->dx0 = 0; f->dy0 = 0; f->dx1 = f->pw; f->dy1 = f->ph; }

/* Present the frame if anything was drawn. The video-mode panel keeps scanning the
 * buffer regardless, so this saves CPU/memory traffic, not panel power; on a
 * command-mode panel the damage rect is what a partial update would send. */
int fb_present(struct fb *f) {
    if (f->dx1 <= f->dx0) return 0;
    if (!f->blanked) fb_flush(f);
    f->frames++;
    f->last_x = f->dx0; f->last_y = f->dy0;
    f->last_w = f->dx1 - f->dx0; f->last_h = f->dy1 - f->dy0;
    f->px_total += (unsigned long)f->last_w * f->last_h;
    f->dx1 = f->dx0; /* empty */
    return 1;
}

int fb_stats(struct fb *f, char *buf, size_t len) {
    return snprintf(buf, len, "frames %lu\npixels %lu\nlast_damage %d %d %d %d\nblanked %d\n",
                    f->frames, f->px_total, f->last_x, f->last_y, f->last_w, f->last_h, f->blanked);
}

void fb_stats_reset(struct fb *f) { f->frames = f->px_total = 0; }

/* on=1: FB_BLANK_POWERDOWN (panel and DSI link off); on=0: unblank and re-present the frame */
static void fb_restore_window(struct fb *f);

int fb_blank(struct fb *f, int on) {
    if (ioctl(f->fd, FBIOBLANK, on ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK) < 0) return -1;
    f->blanked = on;
    /* The panel powers up asynchronously after FB_BLANK_UNBLANK (~230 ms); a single pan right
     * away can be dropped, and with a static source (GUD desktop) nothing would redraw it:
     * backlight on, picture black. Re-present for a second instead. */
    if (!on) { fb_restore_window(f); fb_flush(f); f->repan_next = now_ms() + 100; f->repan_until = now_ms() + 1000; }
    logmsg("fb: %s", on ? "blanked (powerdown)" : "unblanked");
    return 0;
}

/* Re-programs the DECON window. Samsung's decon_pan_display() only updates the DMA source
 * (its decon_set_par() call is #if 0'd), so after a powerdown, which resets the window
 * registers, pans "succeed" but the window stays disabled and the panel shows the black
 * background. A forced FBIOPUT_VSCREENINFO makes fbdev call fb_set_par = decon_set_par. */
static void fb_restore_window(struct fb *f) {
    struct fb_var_screeninfo v = f->var;
    v.yoffset = 0;
    v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(f->fd, FBIOPUT_VSCREENINFO, &v) < 0) logmsg("fb: restore window: %s", strerror(errno));
}

int fb_tick(struct fb *f) {
    if (!f->repan_until) return -1;
    long now = now_ms();
    if (now >= f->repan_until) { f->repan_until = 0; return -1; }
    if (now >= f->repan_next) { fb_restore_window(f); fb_flush(f); f->repan_next = now + 100; }
    return (int)(f->repan_next - now);
}

int fb_open(struct fb *f, const char *path) {
    const char *paths[] = {"/dev/fb0", "/dev/graphics/fb0"};
    ensure_node("graphics", "fb0", "/dev/fb0");
    f->fd = path ? open(path, O_RDWR | O_CLOEXEC) : -1;
    for (unsigned i = 0; i < 2 && f->fd < 0 && !path; i++) f->fd = open(paths[i], O_RDWR | O_CLOEXEC);
    if (f->fd < 0) { logmsg("fb: open failed: %s", strerror(errno)); return -1; }

    if (ioctl(f->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
        logmsg("fb: unblank: %s", strerror(errno));
    if (ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix) < 0 ||
        ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) < 0) {
        logmsg("fb: get info: %s", strerror(errno));
        return -1;
    }
    if (phone.fb_swap_rb) {
        /* the panel shows the framebuffer as BGR although fbdev reports RGB (hw.c, checked with `ppctl colortest`) */
        struct fb_bitfield t = f->var.red; f->var.red = f->var.blue; f->var.blue = t;
        logmsg("fb: red/blue swapped (device quirk)");
    }
    f->pw = f->var.xres; f->ph = f->var.yres; f->bpp = f->var.bits_per_pixel;
    fb_set_rotation(f, 0);
    f->stride = f->fix.line_length ? (int)f->fix.line_length : f->pw * f->bpp / 8;
    f->len = f->fix.smem_len ? f->fix.smem_len : (size_t)f->stride * f->var.yres_virtual;
    logmsg("fb: id=%.16s %dx%d virt %ux%u bpp=%d stride=%d smem=%u",
           f->fix.id, f->pw, f->ph, f->var.xres_virtual, f->var.yres_virtual, f->bpp,
           f->stride, f->fix.smem_len);
    logmsg("fb: rgba offsets r%u/%u g%u/%u b%u/%u a%u/%u  %umm x %umm  pixclock=%u",
           f->var.red.offset, f->var.red.length, f->var.green.offset, f->var.green.length,
           f->var.blue.offset, f->var.blue.length, f->var.transp.offset, f->var.transp.length,
           f->var.width, f->var.height, f->var.pixclock);
    f->mem = mmap(NULL, f->len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->mem == MAP_FAILED) { logmsg("fb: mmap: %s", strerror(errno)); return -1; }
    return 0;
}

void draw_test_pattern(struct fb *f) {
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
    int bh = f->h / 2;
    for (int i = 0; i < 8; i++)
        fb_fill(f, i * f->w / 8, 0, (i + 1) * f->w / 8 - i * f->w / 8, bh,
                fb_color(f, bars[i][0], bars[i][1], bars[i][2]));
    for (int i = 0; i < 32; i++) { /* grey ramp */
        uint8_t v = i * 255 / 31;
        fb_fill(f, i * f->w / 32, bh, f->w / 32 + 1, f->h / 8, fb_color(f, v, v, v));
    }
    /* dark area for touch painting, with a 1px border to check the edges */
    uint32_t bg = fb_color(f, 16, 16, 24), edge = fb_color(f, 255, 64, 64);
    fb_fill(f, 0, bh + f->h / 8, f->w, f->h - bh - f->h / 8, bg);
    fb_fill(f, 0, 0, f->w, 1, edge);
    fb_fill(f, 0, f->h - 1, f->w, 1, edge);
    fb_fill(f, 0, 0, 1, f->h, edge);
    fb_fill(f, f->w - 1, 0, 1, f->h, edge);
    fb_text(f, 40, f->h - 80, 5, "PHONEKIT " PP_VERSION, fb_color(f, 120, 120, 140), bg);
    fb_present(f);
}

/* Status line under the clock (USB mode/state etc.). */
void draw_status(struct fb *f, const char *text) {
    uint32_t bg = fb_color(f, 16, 16, 24), fg = fb_color(f, 150, 200, 255);
    fb_text(f, 40, f->h / 2 + f->h / 8 + 30 + 80, 4, text, fg, bg);
}

/* Status clock: redrawn once per second, damaging only its own rectangle. */
void draw_clock(struct fb *f, const char *text) {
    uint32_t bg = fb_color(f, 16, 16, 24), fg = fb_color(f, 230, 230, 240);
    fb_text(f, 40, f->h / 2 + f->h / 8 + 30, 8, text, fg, bg);
}
