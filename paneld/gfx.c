// Anti-aliased 2D drawing for the PHONEBOX look: an off-screen canvas (logical orientation, 0xRRGGBB pixels),
// rounded rectangles, lines/polylines, filled sparklines, circles, TrueType text (stb_truetype, Roboto,
// glyph cache), images (stb_image), and a blit onto the rotated framebuffer with damage tracking.
#include "paneld.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"
#pragma GCC diagnostic pop

/* fonts embedded by the Makefile (build/fonts.c, from assets/fonts) */
extern const unsigned char font_light[], font_regular[], font_medium[], font_bold[], font_mono[];
static stbtt_fontinfo fonts[GFX_FONTS];
static int fonts_ok;

void gfx_init(void) {
    const unsigned char *src[GFX_FONTS] = {font_light, font_regular, font_medium, font_bold, font_mono};
    fonts_ok = 1;
    for (int i = 0; i < GFX_FONTS; i++)
        if (!stbtt_InitFont(&fonts[i], src[i], stbtt_GetFontOffsetForIndex(src[i], 0))) fonts_ok = 0;
    if (!fonts_ok) logmsg("gfx: font init failed");
}

/* ---------------------------------------------------------------- canvas */

int cv_alloc(struct canvas *c, int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    if (c->cap < (size_t)w * h) {
        free(c->px);
        c->cap = (size_t)w * h;
        c->px = malloc(c->cap * 4);
        if (!c->px) { c->cap = 0; return -1; }
    }
    c->w = w; c->h = h;
    return 0;
}

void cv_clear(struct canvas *c, uint32_t rgb) {
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = rgb;
}

static inline uint32_t mix(uint32_t d, uint32_t s, int a) {
    if (a >= 255) return s;
    if (a <= 0) return d;
    uint32_t dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
    uint32_t sr = s >> 16 & 255, sg = s >> 8 & 255, sb = s & 255;
    dr += ((int)sr - (int)dr) * a / 255;
    dg += ((int)sg - (int)dg) * a / 255;
    db += ((int)sb - (int)db) * a / 255;
    return dr << 16 | dg << 8 | db;
}

static inline void put(struct canvas *c, int x, int y, uint32_t rgb, int a) {
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h || a <= 0) return;
    uint32_t *p = &c->px[(size_t)y * c->w + x];
    *p = mix(*p, rgb, a);
}

uint32_t rgb_mix(uint32_t a, uint32_t b, int t) { return mix(a, b, t); }

void cv_rect(struct canvas *c, int x, int y, int w, int h, uint32_t rgb, int a) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < c->h; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < c->w; i++) put(c, i, j, rgb, a);
}

/* Coverage of pixel (px,py) by a rounded rectangle, 0..1 (signed-distance based AA). */
static float rr_cov(float px, float py, float x, float y, float w, float h, float r) {
    float cx = px < x + r ? x + r : px > x + w - r ? x + w - r : px;
    float cy = py < y + r ? y + r : py > y + h - r ? y + h - r : py;
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy) - r; /* <0 inside the corner circle */
    if (px >= x + r && px <= x + w - r) d = fmaxf(y - py, py - (y + h));
    else if (py >= y + r && py <= y + h - r) d = fmaxf(x - px, px - (x + w));
    float cov = 0.5f - d;
    return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

void cv_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t rgb, int a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        int corner_row = j < y + r || j >= y + h - r;
        for (int i = x; i < x + w; i++) {
            if (!corner_row && i >= x + r && i < x + w - r) { put(c, i, j, rgb, a); continue; }
            float cov = rr_cov(i + 0.5f, j + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
            put(c, i, j, rgb, (int)(a * cov));
        }
    }
}

/* 1-px-ish border: outer rrect coverage minus inner rrect coverage. */
void cv_rrect_stroke(struct canvas *c, int x, int y, int w, int h, int r, float t, uint32_t rgb, int a) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        for (int i = x; i < x + w; i++) {
            float px = i + 0.5f, py = j + 0.5f;
            if (px > x + r + t && px < x + w - r - t && py > y + t + 1 && py < y + h - t - 1) { i = x + w - r - (int)t - 2; continue; }
            float o = rr_cov(px, py, (float)x, (float)y, (float)w, (float)h, (float)r);
            float in = rr_cov(px, py, x + t, y + t, w - 2 * t, h - 2 * t, r - t > 0 ? r - t : 0);
            float cov = o - in;
            if (cov > 0) put(c, i, j, rgb, (int)(a * cov));
        }
    }
}

void cv_circle(struct canvas *c, float cx, float cy, float r, uint32_t rgb, int a) {
    for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++)
        for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov > 0) put(c, i, j, rgb, (int)(a * (cov > 1 ? 1 : cov)));
        }
}

/* Thick anti-aliased segment (distance to segment). */
void cv_line(struct canvas *c, float x0, float y0, float x1, float y1, float th, uint32_t rgb, int a) {
    float hw = th / 2;
    int minx = (int)floorf(fminf(x0, x1) - hw - 1), maxx = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int miny = (int)floorf(fminf(y0, y1) - hw - 1), maxy = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
    for (int j = miny; j <= maxy; j++)
        for (int i = minx; i <= maxx; i++) {
            float px = i + 0.5f - x0, py = j + 0.5f - y0;
            float t = l2 > 0 ? (px * vx + py * vy) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - t * vx, dy = py - t * vy;
            float cov = hw + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov > 0) {
                uint32_t *p = (unsigned)i < (unsigned)c->w && (unsigned)j < (unsigned)c->h ? &c->px[(size_t)j * c->w + i] : NULL;
                if (p) *p = mix(*p, rgb, (int)(a * (cov > 1 ? 1 : cov)));
            }
        }
}

/* Sparkline: area under the curve with a vertical fade, then the line on top.
 * v[0..n) in [0,1] (oldest first), drawn into the box (x,y,w,h). */
void cv_spark(struct canvas *c, int x, int y, int w, int h, const float *v, int n, uint32_t rgb, int line_a, int fill_a, float th) {
    if (n < 2) return;
    float step = (float)w / (n - 1);
    for (int i = 0; i < w; i++) {
        float fi = i / step;
        int k = (int)fi;
        if (k >= n - 1) k = n - 2;
        float t = fi - k, val = v[k] + (v[k + 1] - v[k]) * t;
        float top = y + h - val * h;
        for (int j = (int)top; j < y + h; j++) {
            float f = (float)(j - top) / (h > 0 ? h : 1);
            put(c, x + i, j, rgb, (int)(fill_a * (1 - f)));
        }
    }
    for (int k = 0; k < n - 1; k++)
        cv_line(c, x + k * step, y + h - v[k] * h, x + (k + 1) * step, y + h - v[k + 1] * h, th, rgb, line_a);
}

/* ---------------------------------------------------------------- text */

#define GCACHE 1024
static struct glyph { int font, size, cp; int w, h, xoff, yoff, adv; unsigned char *bm; unsigned long used; } gc[GCACHE];
static unsigned long gtick;

static struct glyph *glyph(int font, int size, int cp) {
    unsigned h = (unsigned)(cp * 31 + size * 7 + font) % GCACHE;
    for (int k = 0; k < 8; k++) {
        struct glyph *g = &gc[(h + k) % GCACHE];
        if (g->bm && g->font == font && g->size == size && g->cp == cp) { g->used = ++gtick; return g; }
    }
    struct glyph *v = &gc[h];
    for (int k = 1; k < 8; k++) if (!gc[(h + k) % GCACHE].bm || gc[(h + k) % GCACHE].used < v->used) v = &gc[(h + k) % GCACHE];
    free(v->bm);
    memset(v, 0, sizeof *v);
    stbtt_fontinfo *f = &fonts[font];
    float sc = stbtt_ScaleForPixelHeight(f, (float)size);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
    v->bm = stbtt_GetCodepointBitmap(f, 0, sc, cp, &v->w, &v->h, &v->xoff, &v->yoff);
    if (!v->bm) v->bm = calloc(1, 1);
    v->adv = (int)lroundf(adv * sc * 64); /* 26.6 fixed point */
    v->font = font; v->size = size; v->cp = cp; v->used = ++gtick;
    return v;
}

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++;
    if (cp >= 0xf0 && p[0] && p[1] && p[2]) { cp = (cp & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (cp >= 0xe0 && p[0] && p[1]) { cp = (cp & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (cp >= 0xc0 && p[0]) { cp = (cp & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return cp;
}

/* Width in px of `s` at `size`, with extra letter spacing `ls` (px, may be negative). */
int cv_text_w(int font, int size, const char *s, float ls) {
    if (!fonts_ok) return 0;
    long w = 0;
    int n = 0;
    while (*s) {
        int cp = utf8_next(&s);
        struct glyph *g = glyph(font, size, cp);
        w += g->adv;
        n++;
    }
    return (int)(w / 64 + ls * (n > 0 ? n - 1 : 0));
}

/* Draws `s` with its baseline at y; returns the x after the last glyph. Clips at max_w (ellipsis-free). */
int cv_text(struct canvas *c, int font, int size, int x, int y, const char *s, uint32_t rgb, int a, float ls, int max_w) {
    if (!fonts_ok) return x;
    long pen = (long)x * 64;
    float extra = 0;
    const char *t = s;
    while (*t) {
        int cp = utf8_next(&t);
        struct glyph *g = glyph(font, size, cp);
        int gx = (int)(pen / 64 + extra) + g->xoff;
        if (max_w > 0 && gx + g->w > x + max_w) break;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int cov = g->bm[j * g->w + i];
                if (cov) put(c, gx + i, y + g->yoff + j, rgb, cov * a / 255);
            }
        pen += g->adv;
        extra += ls;
    }
    return (int)(pen / 64 + extra);
}

/* Font metrics for layout: ascent in px for `size` (cap-ish height ≈ 0.72 * size for Roboto). */
int cv_ascent(int font, int size) {
    if (!fonts_ok) return size;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fonts[font], &asc, &desc, &gap);
    return (int)(asc * stbtt_ScaleForPixelHeight(&fonts[font], (float)size));
}

/* ---------------------------------------------------------------- images */

/* Decodes a JPEG/PNG into a new 0xRRGGBB canvas scaled to w x h (cover crop). 0 = ok. */
int cv_image(struct canvas *out, const unsigned char *data, int len, int w, int h) {
    int iw, ih, n;
    unsigned char *img = stbi_load_from_memory(data, len, &iw, &ih, &n, 3);
    if (!img) return -1;
    if (cv_alloc(out, w, h) < 0) { stbi_image_free(img); return -1; }
    float s = fmaxf((float)w / iw, (float)h / ih);
    float ox = (iw - w / s) / 2, oy = (ih - h / s) / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = (int)(ox + i / s), sy = (int)(oy + j / s);
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx >= iw) sx = iw - 1;
            if (sy >= ih) sy = ih - 1;
            const unsigned char *p = img + ((size_t)sy * iw + sx) * 3;
            out->px[(size_t)j * w + i] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        }
    stbi_image_free(img);
    return 0;
}

/* Copies `src` into c at (x,y) through a rounded-rect mask of radius r. */
void cv_blit_rounded(struct canvas *c, const struct canvas *src, int x, int y, int r) {
    for (int j = 0; j < src->h; j++)
        for (int i = 0; i < src->w; i++) {
            float cov = rr_cov(i + 0.5f, j + 0.5f, 0, 0, (float)src->w, (float)src->h, (float)r);
            put(c, x + i, y + j, src->px[(size_t)j * src->w + i], (int)(255 * cov));
        }
}

/* ---------------------------------------------------------------- to the framebuffer */

/* Blits a canvas at logical (lx, ly), rotating onto the panel, and adds it to the damage. */
void fb_blit(struct fb *f, const struct canvas *c, int lx, int ly) {
    const struct fb_var_screeninfo *v = &f->var;
    int ro = v->red.offset, go = v->green.offset, bo = v->blue.offset;
    uint32_t alpha = v->transp.length ? ((1u << v->transp.length) - 1) << v->transp.offset : 0;
    int bypp = f->bpp / 8;
    for (int j = 0; j < c->h; j++) {
        int y = ly + j;
        if (y < 0 || y >= f->h) continue;
        for (int i = 0; i < c->w; i++) {
            int x = lx + i;
            if (x < 0 || x >= f->w) continue;
            int px, py;
            switch (f->rot) {
            case 90:  px = f->pw - 1 - y; py = x; break;
            case 180: px = f->pw - 1 - x; py = f->ph - 1 - y; break;
            case 270: px = y; py = f->ph - 1 - x; break;
            default:  px = x; py = y;
            }
            uint32_t s = c->px[(size_t)j * c->w + i];
            uint8_t *dst = f->mem + (size_t)py * f->stride + (size_t)px * bypp;
            if (bypp == 4) {
                *(uint32_t *)dst = (s >> 16 & 255) << ro | (s >> 8 & 255) << go | (s & 255) << bo | alpha;
            } else if (bypp == 2) {
                *(uint16_t *)dst = (uint16_t)(((s >> 19) & 31) << ro | ((s >> 10) & 63) << go | ((s >> 3) & 31) << bo);
            }
        }
    }
    fb_damage_rect(f, lx, ly, c->w, c->h);
}
