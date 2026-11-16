// PPJ/1 widget engine for PANEL mode (protocol/PPJ1.md).
// Layouts are grids of cells; each `set` redraws only that widget's rectangle.
#include "paneld.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGES 8
#define WIDGETS 48
#define GRAPH_N 60

enum { W_LABEL, W_VALUE, W_GAUGE, W_GRAPH, W_BUTTON, W_TOGGLE, W_SLIDER, W_CLOCK, W_TYPES };
static const char *const type_names[W_TYPES] = {"label", "value", "gauge", "graph", "button", "toggle", "slider", "clock"};

struct widget {
    char id[24];
    int type, x, y, w, h, size, decimals, has_minmax, hms;
    char text[48], label[32], unit[12];
    double value, min, max;
    uint8_t cr, cg, cb; /* accent */
    float g[GRAPH_N];
    int gn, gpos;
    int pressed;
};

static struct page { char title[32]; int n; struct widget w[WIDGETS]; } pages[PAGES];
static int cur, have_layout;
static struct fb *fb;
static void (*event_sink)(const char *json);

void panel_set_event_sink(void (*fn)(const char *json)) { event_sink = fn; }
int panel_has_layout(void) { return have_layout; }

static int cols(void) { return fb->w > fb->h ? 12 : 6; }
static int rows(void) { return fb->w > fb->h ? 6 : 12; }
#define TITLE_H 90

static void cell_rect(const struct widget *w, int *x, int *y, int *ww, int *hh) {
    int cw = fb->w / cols(), ch = (fb->h - TITLE_H) / rows();
    *x = w->x * cw; *y = TITLE_H + w->y * ch; *ww = w->w * cw; *hh = w->h * ch;
}

/* ---------------------------------------------------------------- drawing */

static uint32_t C(int r, int g, int b) { return fb_color(fb, r, g, b); }

static void text_fit(int x, int y, int maxw, int maxh, int maxscale, const char *s, uint32_t fg, uint32_t bg) {
    int len = (int)strlen(s);
    if (!len) return;
    int sc = maxscale;
    while (sc > 1 && (len * 6 * sc > maxw || 8 * sc > maxh)) sc--;
    fb_text(fb, x, y, sc, s, fg, bg);
}

static void fmt_value(const struct widget *w, char *b, size_t n) {
    snprintf(b, n, "%.*f%s%s", w->decimals, w->value, w->unit[0] ? " " : "", w->unit);
}

static void draw_widget(struct widget *w) {
    int x, y, ww, hh;
    cell_rect(w, &x, &y, &ww, &hh);
    const int m = 8; /* margin between cells */
    x += m; y += m; ww -= 2 * m; hh -= 2 * m;
    if (ww <= 0 || hh <= 0) return;
    uint32_t card = C(28, 32, 44), fg = C(235, 238, 245), dim = C(130, 140, 160), acc = C(w->cr, w->cg, w->cb);
    fb_fill(fb, x, y, ww, hh, card);
    int pad = 16, cap = hh > 120 ? 36 : 0;
    if (cap && w->label[0]) text_fit(x + pad, y + pad, ww - 2 * pad, cap, 4, w->label, dim, card);
    char b[64];
    switch (w->type) {
    case W_LABEL: text_fit(x + pad, y + (hh - 8 * w->size * 3) / 2, ww - 2 * pad, hh - 2 * pad, w->size * 3, w->text, fg, card); break;
    case W_VALUE:
        fmt_value(w, b, sizeof b);
        text_fit(x + pad, y + cap + pad, ww - 2 * pad, hh - cap - 2 * pad, 14, b, acc, card);
        break;
    case W_GAUGE: {
        double f = w->max > w->min ? (w->value - w->min) / (w->max - w->min) : 0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int by = y + cap + pad, bh = hh - cap - 2 * pad;
        int numw = ww / 3;
        fb_fill(fb, x + pad, by, ww - 2 * pad - numw, bh, C(45, 50, 66));
        fb_fill(fb, x + pad, by, (int)((ww - 2 * pad - numw) * f), bh, acc);
        fmt_value(w, b, sizeof b);
        text_fit(x + ww - numw, by, numw - pad, bh, 8, b, fg, card);
        break;
    }
    case W_GRAPH: {
        int gx = x + pad, gy = y + cap + pad, gw = ww - 2 * pad, gh = hh - cap - 2 * pad;
        fb_fill(fb, gx, gy, gw, gh, C(20, 23, 32));
        if (w->gn < 1) break;
        double lo = w->min, hi = w->max;
        if (!w->has_minmax) {
            lo = 1e300; hi = -1e300;
            for (int i = 0; i < w->gn; i++) { double v = w->g[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
            if (hi - lo < 1e-9) { hi += 1; lo -= 1; }
        }
        int bw = gw / GRAPH_N > 0 ? gw / GRAPH_N : 1;
        for (int i = 0; i < w->gn; i++) {
            int idx = (w->gpos - w->gn + i + GRAPH_N) % GRAPH_N;
            double f = (w->g[idx] - lo) / (hi - lo);
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            int bhh = (int)(gh * f);
            fb_fill(fb, gx + (GRAPH_N - w->gn + i) * gw / GRAPH_N, gy + gh - bhh, bw, bhh > 0 ? bhh : 1, acc);
        }
        snprintf(b, sizeof b, "%.*f", w->decimals, w->g[(w->gpos + GRAPH_N - 1) % GRAPH_N]);
        text_fit(gx + 8, gy + 8, gw / 2, 40, 4, b, fg, C(20, 23, 32));
        break;
    }
    case W_BUTTON: {
        uint32_t bg = w->pressed ? acc : C(45, 52, 72);
        fb_fill(fb, x, y, ww, hh, bg);
        int len = (int)strlen(w->text), sc = 8;
        while (sc > 1 && (len * 6 * sc > ww - 2 * pad || 8 * sc > hh - 2 * pad)) sc--;
        fb_text(fb, x + (ww - len * 6 * sc) / 2, y + (hh - 8 * sc) / 2, sc, w->text, fg, bg);
        break;
    }
    case W_TOGGLE: {
        uint32_t bg = w->value ? acc : C(45, 52, 72);
        fb_fill(fb, x, y, ww, hh, bg);
        snprintf(b, sizeof b, "%s %s", w->text, w->value ? "ON" : "OFF");
        int len = (int)strlen(b), sc = 8;
        while (sc > 1 && (len * 6 * sc > ww - 2 * pad || 8 * sc > hh - 2 * pad)) sc--;
        fb_text(fb, x + (ww - len * 6 * sc) / 2, y + (hh - 8 * sc) / 2, sc, b, fg, bg);
        break;
    }
    case W_SLIDER: {
        double f = w->max > w->min ? (w->value - w->min) / (w->max - w->min) : 0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int ty = y + cap + pad, th = hh - cap - 2 * pad;
        fb_fill(fb, x + pad, ty + th / 2 - 6, ww - 2 * pad, 12, C(45, 50, 66));
        fb_fill(fb, x + pad, ty + th / 2 - 6, (int)((ww - 2 * pad) * f), 12, acc);
        int kx = x + pad + (int)((ww - 2 * pad - 40) * f);
        fb_fill(fb, kx, ty + th / 2 - 30, 40, 60, fg);
        break;
    }
    case W_CLOCK: {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        if (w->hms) snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        else snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
        text_fit(x + pad, y + cap + pad, ww - 2 * pad, hh - cap - 2 * pad, 14, b, fg, card);
        break;
    }
    }
}

void panel_draw(void) {
    if (!fb || !fb->mem) return;
    uint32_t bg = C(10, 12, 18);
    fb_fill(fb, 0, 0, fb->w, fb->h, bg);
    struct page *p = &pages[cur];
    char t[48];
    snprintf(t, sizeof t, "%s", p->title[0] ? p->title : "PANEL");
    fb_text(fb, 24, 24, 6, t, C(200, 205, 220), bg);
    int np = 0;
    for (int i = 0; i < PAGES; i++) if (pages[i].n) np++;
    if (np > 1) { snprintf(t, sizeof t, "%d/%d", cur + 1, np); fb_text(fb, fb->w - 24 - (int)strlen(t) * 24, 30, 4, t, C(120, 130, 155), bg); }
    for (int i = 0; i < p->n; i++) draw_widget(&p->w[i]);
    if (!np) { /* nothing sent yet: say how to reach us */
        uint32_t dim = C(120, 130, 155);
        fb_text(fb, 24, fb->h / 2 - 60, 5, "WAITING FOR HOST", C(200, 205, 220), bg);
        fb_text(fb, 24, fb->h / 2 + 10, 3, "PPJ/1  TCP 10.42.0.2:7700", dim, bg);
    }
    fb_present(fb);
}

static int visible(void) { return hm_owner() == OWN_PAGE && ui_page() == UI_PAGE_PANEL; }

static void redraw_one(struct widget *w) {
    if (!visible()) return;
    draw_widget(w);
    fb_present(fb);
}

/* ---------------------------------------------------------------- protocol */

static int parse_color(const char *s, struct widget *w) {
    unsigned v;
    if (s[0] == '#' && strlen(s) == 7 && sscanf(s + 1, "%x", &v) == 1) {
        w->cr = v >> 16; w->cg = (v >> 8) & 255; w->cb = v & 255;
        return 0;
    }
    return -1;
}

static void apply_fields(const char *s, const jtok *t, int n, int o, struct widget *w) {
    int k;
    if ((k = json_get(s, t, n, o, "text")) >= 0) json_str(s, t, k, w->text, sizeof w->text);
    if ((k = json_get(s, t, n, o, "label")) >= 0) json_str(s, t, k, w->label, sizeof w->label);
    if ((k = json_get(s, t, n, o, "unit")) >= 0) json_str(s, t, k, w->unit, sizeof w->unit);
    if ((k = json_get(s, t, n, o, "value")) >= 0) w->value = json_num(s, t, k, w->value);
    if ((k = json_get(s, t, n, o, "min")) >= 0) { w->min = json_num(s, t, k, 0); w->has_minmax = 1; }
    if ((k = json_get(s, t, n, o, "max")) >= 0) { w->max = json_num(s, t, k, 100); w->has_minmax = 1; }
    if ((k = json_get(s, t, n, o, "decimals")) >= 0) w->decimals = (int)json_num(s, t, k, 0);
    if ((k = json_get(s, t, n, o, "size")) >= 0) w->size = (int)json_num(s, t, k, 2);
    if ((k = json_get(s, t, n, o, "push")) >= 0) {
        w->g[w->gpos] = (float)json_num(s, t, k, 0);
        w->gpos = (w->gpos + 1) % GRAPH_N;
        if (w->gn < GRAPH_N) w->gn++;
    }
    char c[16];
    if ((k = json_get(s, t, n, o, "color")) >= 0 && json_str(s, t, k, c, sizeof c) > 0) parse_color(c, w);
    if ((k = json_get(s, t, n, o, "format")) >= 0 && json_str(s, t, k, c, sizeof c) > 0) w->hms = strcmp(c, "hm") != 0;
    if (w->decimals < 0) w->decimals = 0;
    if (w->decimals > 4) w->decimals = 4;
    if (w->size < 1) w->size = 1;
    if (w->size > 4) w->size = 4;
}

static struct widget *find(const char *id, int *pg) {
    for (int p = 0; p < PAGES; p++)
        for (int i = 0; i < pages[p].n; i++)
            if (!strcmp(pages[p].w[i].id, id)) { if (pg) *pg = p; return &pages[p].w[i]; }
    return NULL;
}

static int err(char *r, size_t n, const char *msg) { return snprintf(r, n, "{\"op\":\"error\",\"msg\":\"%s\"}", msg); }

int panel_info(char *r, size_t n) {
    int np = 0;
    for (int i = 0; i < PAGES; i++) if (pages[i].n) np++;
    return snprintf(r, n, "{\"op\":\"info\",\"version\":\"PPJ/1 %s\",\"cols\":%d,\"rows\":%d,\"w\":%d,\"h\":%d,"
                    "\"rotation\":%d,\"page\":%d,\"pages\":%d}", PP_VERSION, cols(), rows(), fb->w, fb->h, fb->rot, cur, np);
}

int panel_handle(const char *s, int len, char *r, size_t rn) {
    static jtok t[1024];
    r[0] = 0;
    int n = json_parse(s, len, t, 1024);
    if (n < 1 || t[0].type != J_OBJ) return err(r, rn, "bad json");
    char op[16] = "";
    json_str(s, t, json_get(s, t, n, 0, "op"), op, sizeof op);
    if (!strcmp(op, "ping")) return snprintf(r, rn, "{\"op\":\"pong\"}");
    if (!strcmp(op, "info")) return panel_info(r, rn);
    if (!strcmp(op, "layout")) {
        int pg = (int)json_num(s, t, json_get(s, t, n, 0, "page"), 0);
        if (pg < 0 || pg >= PAGES) return err(r, rn, "page out of range");
        struct page *p = &pages[pg];
        memset(p, 0, sizeof *p);
        json_str(s, t, json_get(s, t, n, 0, "title"), p->title, sizeof p->title);
        int arr = json_get(s, t, n, 0, "widgets");
        if (arr >= 0 && t[arr].type == J_ARR) {
            int j = arr + 1;
            for (int k = 0; k < t[arr].size && j < n && p->n < WIDGETS; k++) {
                if (t[j].type == J_OBJ) {
                    struct widget *w = &p->w[p->n];
                    memset(w, 0, sizeof *w);
                    w->cr = 90; w->cg = 160; w->cb = 255; w->max = 100; w->size = 2; w->hms = 1;
                    json_str(s, t, json_get(s, t, n, j, "id"), w->id, sizeof w->id);
                    char ty[16] = "";
                    json_str(s, t, json_get(s, t, n, j, "type"), ty, sizeof ty);
                    w->type = -1;
                    for (int q = 0; q < W_TYPES; q++) if (!strcmp(ty, type_names[q])) w->type = q;
                    w->x = (int)json_num(s, t, json_get(s, t, n, j, "x"), 0);
                    w->y = (int)json_num(s, t, json_get(s, t, n, j, "y"), 0);
                    w->w = (int)json_num(s, t, json_get(s, t, n, j, "w"), 1);
                    w->h = (int)json_num(s, t, json_get(s, t, n, j, "h"), 1);
                    apply_fields(s, t, n, j, w);
                    int okw = w->type >= 0 && w->w > 0 && w->h > 0 && w->x >= 0 && w->y >= 0 &&
                              w->x + w->w <= 12 && w->y + w->h <= 12;
                    if (okw) p->n++;
                }
                j = json_skip(t, n, j);
            }
        }
        have_layout = 1;
        cur = pg;
        if (ui_page() != UI_PAGE_PANEL && hm_owner() == OWN_PAGE) ui_page_set(UI_PAGE_PANEL);
        else if (visible()) panel_draw();
        return snprintf(r, rn, "{\"op\":\"ok\",\"page\":%d,\"widgets\":%d}", pg, p->n);
    }
    if (!strcmp(op, "set")) {
        char id[24] = "";
        json_str(s, t, json_get(s, t, n, 0, "id"), id, sizeof id);
        int pg;
        struct widget *w = find(id, &pg);
        if (!w) return err(r, rn, "unknown id");
        apply_fields(s, t, n, 0, w);
        if (pg == cur) redraw_one(w);
        return 0; /* no reply for set: keeps the stream cheap */
    }
    if (!strcmp(op, "page")) {
        int pg = (int)json_num(s, t, json_get(s, t, n, 0, "n"), 0);
        if (pg < 0 || pg >= PAGES || !pages[pg].n) return err(r, rn, "no such page");
        cur = pg;
        if (visible()) panel_draw();
        return snprintf(r, rn, "{\"op\":\"ok\",\"page\":%d}", cur);
    }
    if (!strcmp(op, "clear")) {
        int k = json_get(s, t, n, 0, "page");
        if (k >= 0) { int pg = (int)json_num(s, t, k, 0); if (pg >= 0 && pg < PAGES) memset(&pages[pg], 0, sizeof pages[pg]); }
        else memset(pages, 0, sizeof pages);
        have_layout = 0;
        for (int i = 0; i < PAGES; i++) if (pages[i].n) have_layout = 1;
        if (!have_layout && ui_page() == UI_PAGE_PANEL) ui_page_set(UI_PAGE_STATUS);
        else if (visible()) panel_draw();
        return snprintf(r, rn, "{\"op\":\"ok\"}");
    }
    return err(r, rn, "unknown op");
}

/* ---------------------------------------------------------------- touch */

static void send_event(const struct widget *w, const char *type, int with_value) {
    if (!event_sink) return;
    char b[160];
    if (with_value) snprintf(b, sizeof b, "{\"op\":\"event\",\"id\":\"%s\",\"type\":\"%s\",\"value\":%g}", w->id, type, w->value);
    else snprintf(b, sizeof b, "{\"op\":\"event\",\"id\":\"%s\",\"type\":\"%s\"}", w->id, type);
    event_sink(b);
}

static int touch_id = -1;
static struct widget *touched;
static long last_slider_ms;

static struct widget *hit(int x, int y) {
    struct page *p = &pages[cur];
    for (int i = 0; i < p->n; i++) {
        int wx, wy, ww, wh;
        cell_rect(&p->w[i], &wx, &wy, &ww, &wh);
        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) return &p->w[i];
    }
    return NULL;
}

static void slider_to(struct widget *w, int x) {
    int wx, wy, ww, wh;
    cell_rect(w, &wx, &wy, &ww, &wh);
    double f = (double)(x - wx - 24) / (ww - 48);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    w->value = w->min + f * (w->max - w->min);
    redraw_one(w);
}

int panel_touch(const int *ids, const int *xs, const int *ys, int n) {
    int idx = -1;
    for (int i = 0; i < n; i++) if (ids[i] == touch_id) idx = i;
    if (touch_id >= 0 && idx < 0) { /* lifted */
        struct widget *w = touched;
        touch_id = -1; touched = NULL;
        if (w && w->type == W_BUTTON) { w->pressed = 0; redraw_one(w); send_event(w, "press", 0); }
        else if (w && w->type == W_TOGGLE) { w->value = !w->value; redraw_one(w); send_event(w, "change", 1); }
        else if (w && w->type == W_SLIDER) send_event(w, "change", 1);
        return 1;
    }
    if (touch_id < 0 && n > 0) {
        touch_id = ids[0];
        touched = hit(xs[0], ys[0]);
        if (touched && touched->type == W_BUTTON) { touched->pressed = 1; redraw_one(touched); }
        if (touched && touched->type == W_SLIDER) slider_to(touched, xs[0]);
        return 1;
    }
    if (idx >= 0 && touched && touched->type == W_SLIDER) {
        slider_to(touched, xs[idx]);
        if (now_ms() - last_slider_ms > 100) { last_slider_ms = now_ms(); send_event(touched, "change", 1); }
    }
    return 1;
}

int panel_page_step(int d) {
    for (int k = 1; k <= PAGES; k++) {
        int p = cur + d * k;
        if (p < 0 || p >= PAGES) return 1; /* wrapped out of the panel pages */
        if (pages[p].n) { cur = p; panel_draw(); return 0; }
    }
    return 1;
}

void panel_tick(void) {
    if (!visible()) return;
    struct page *p = &pages[cur];
    int any = 0;
    for (int i = 0; i < p->n; i++) if (p->w[i].type == W_CLOCK) { draw_widget(&p->w[i]); any = 1; }
    if (any) fb_present(fb);
}

void panel_init(struct fb *f) { fb = f; }
