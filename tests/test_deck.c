// Host test for DECK mode and the drawing it relies on (ASan + UBSan in CI):
//  - cv_rrect_stroke with fractional border widths terminates (it once looped forever and starved the watchdog)
//  - the deck draws in landscape and portrait, two held buttons merge into one keyboard report, release clears it
//  - glossy, glass and flat styles, multi-line labels and hidden hints draw in both orientations
//  - actions parse: combos, sequences, text, media keys; bad key names are rejected
//   gcc -fsanitize=address,undefined -g -D_GNU_SOURCE -Ipaneld tests/test_deck.c paneld/deck.c paneld/gfx.c \
//       out/<device>/fonts.c -lm -o test_deck && ./test_deck
#include "paneld.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long now_ms(void) { return 0; }
void logmsg(const char *f, ...) { (void)f; }
const char *store_get(const char *k, const char *d) { (void)k; return d; }
int store_set(const char *k, const char *v) { (void)k; (void)v; return 0; }
int store_unset(const char *k) { (void)k; return 0; }
char gadget_mode(void) { return 'A'; }
const char *gadget_state(void) { return "configured"; }
int bthid_connected(void) { return 0; }
int hm_owner(void) { return OWN_INPUT; }
int hm_input_mode(void) { return IN_DECK; }
void fb_damage_rect(struct fb *f, int a, int b, int c, int d) { (void)f; (void)a; (void)b; (void)c; (void)d; }
int fb_present(struct fb *f) { (void)f; return 0; }

static char log_[4096];
static void logf_(const char *fmt, ...) {
    size_t l = strlen(log_);
    va_list a;
    va_start(a, fmt);
    vsnprintf(log_ + l, sizeof log_ - l, fmt, a);
    va_end(a);
}
void hm_kbd_raw(int m, const unsigned char k[6]) { logf_("k%02x:%02x%02x;", m, k[0], k[1]); }
void hm_consumer(int u) { logf_("c%x;", u); }
void type_hid_text(const char *s) { logf_("t%s;", s); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

static void on_alarm(int s) { (void)s; puts("FAIL: timed out (endless loop?)"); _exit(1); }

int main(void) {
    signal(SIGALRM, on_alarm);
    alarm(60);
    gfx_init();
    struct canvas c = {0};
    cv_alloc(&c, 600, 400);
    for (float t = 0.5f; t < 6.0f; t += 0.1f) cv_rrect_stroke(&c, 30, 40, 552, 288, 48, t, 0x60a5fa, 110);

    struct fb f;
    memset(&f, 0, sizeof f);
    int dims[2][2] = {{2340, 1080}, {1080, 2340}};
    for (int d = 0; d < 2; d++) {
        f.w = f.pw = dims[d][0]; f.h = f.ph = dims[d][1];
        f.bpp = 32; f.stride = f.w * 4;
        f.var.red.offset = 16; f.var.green.offset = 8;
        free(f.mem);
        f.mem = calloc((size_t)f.w * f.h, 4);
        deck_draw(&f);
        /* button 1 (top left) and button 2 (next to it) held together, then lifted */
        int ids[2] = {1, 2}, xs[2] = {f.w / 8, f.w / 8 + f.w / (f.w > f.h ? 6 : 3)}, ys[2] = {f.h / 4, f.h / 4};
        log_[0] = 0;
        deck_touch(&f, ids, xs, ys, 1);
        deck_touch(&f, ids, xs, ys, 2);
        deck_touch(&f, ids, xs, ys, 0);
        CHECK(strstr(log_, "k00:6800;") && strstr(log_, "k00:6869;") && strstr(log_, "k00:0000;"), "merge/release: %s", log_);
        /* every style draws, with a lit toggle, a pressed key and a three-line label */
        char o2[4096];
        char *lbl[] = {"deck", "set", "9", "BE\\nRIGHT\\nBACK", "f21", "violet"};
        deck_ctl(lbl, 6, "", o2, sizeof o2);
        char *tog[] = {"deck", "press", "5"};
        deck_ctl(tog, 3, "", o2, sizeof o2);
        const char *styles[] = {"glass", "flat", "nope", "glossy"};
        for (int k = 0; k < 4; k++) {
            char *st[] = {"deck", "style", (char *)styles[k]};
            deck_ctl(st, 3, "", o2, sizeof o2);
            CHECK(k == 2 ? !!strstr(o2, "error") : !!strstr(o2, styles[k]), "style %s: %s", styles[k], o2);
            deck_draw(&f);
            deck_touch(&f, ids, xs, ys, 1);
            deck_touch(&f, ids, xs, ys, 0);
        }
        char *hi[] = {"deck", "hints", "off"};
        deck_ctl(hi, 3, "", o2, sizeof o2);
        deck_draw(&f);
        hi[2] = "on";
        deck_ctl(hi, 3, "", o2, sizeof o2);
        char *rs[] = {"deck", "reset", "all"};
        deck_ctl(rs, 3, "", o2, sizeof o2);
    }
    char out[4096];
    char *set1[] = {"deck", "set", "3", "COPY+PASTE", "ctrl+c ctrl+v", "pink"};
    CHECK(strstr((deck_ctl(set1, 6, "", out, sizeof out), out), "button 3"), "set sequence: %s", out);
    char *press3[] = {"deck", "press", "3"};
    log_[0] = 0;
    deck_ctl(press3, 3, "", out, sizeof out);
    CHECK(!strcmp(log_, "k01:0600;k00:0000;k01:1900;k00:0000;"), "sequence: %s", log_);
    char *set2[] = {"deck", "set", "4", "PLAY", "media:play"};
    deck_ctl(set2, 5, "", out, sizeof out);
    char *press4[] = {"deck", "press", "4"};
    log_[0] = 0;
    deck_ctl(press4, 3, "", out, sizeof out);
    CHECK(!strcmp(log_, "ccd;c0;"), "media: %s", log_);
    char *bad[] = {"deck", "set", "5", "X", "ctrl+nosuchkey"};
    deck_ctl(bad, 5, "", out, sizeof out);
    CHECK(!strncmp(out, "error", 5), "bad key accepted: %s", out);
    char *set3[] = {"deck", "set", "6", "GG", "text:gg wp", "toggle"};
    deck_ctl(set3, 6, "", out, sizeof out);
    char *press6[] = {"deck", "press", "6"};
    log_[0] = 0;
    deck_ctl(press6, 3, "", out, sizeof out);
    CHECK(!strcmp(log_, "tgg wp;"), "text: %s", log_);
    free(f.mem);
    free(c.px);
    printf("%s\n", fails ? "test_deck: FAILED" : "test_deck: ok");
    return fails != 0;
}
