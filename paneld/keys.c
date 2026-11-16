// Hardware keys -> configurable actions (short press / long press per key).
//
// Bindings live in the settings store: key.<name>.short / key.<name>.long = <action>.
// Names: power, volup, voldown, xcover (0xfc), top (KEY_MACRO17), or key<code>.
// Actions: none, screen_toggle, screen_off, page_next, page_prev, mode_next, mode_prev, menu,
//          mode_toggle, mode_a, mode_b, bright_up, bright_down, status_page.
// While the mode menu is open: vol/top move, xcover/power select.
// Fixed safety behaviour: holding power for 3 s always reboots. While the screen is
// off, the first press of any key only wakes it.
#include "paneld.h"

#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#define KEY_XCOVER 0xfc
#define LONG_MS 800
#define MAX_KEYS_TRACKED 8

static const struct { int code; const char *name; const char *dshort, *dlong; } known[] = {
    {KEY_POWER, "power", "screen_toggle", "none"},   /* long = 3 s reboot, handled separately */
    {KEY_VOLUMEUP, "volup", "bright_up", "page_prev"},
    {KEY_VOLUMEDOWN, "voldown", "bright_down", "page_next"},
    {KEY_XCOVER, "xcover", "mode_next", "menu"},
    {KEY_MACRO17, "top", "page_next", "page_prev"},
};

static struct { int code; long down_ms; int woke; } held[MAX_KEYS_TRACKED];

static const char *key_name(int code, char *buf, size_t len) {
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].code == code) return known[i].name;
    snprintf(buf, len, "key%d", code);
    return buf;
}

static const char *binding(int code, int is_long) {
    char nb[16], k[48];
    const char *name = key_name(code, nb, sizeof nb);
    snprintf(k, sizeof k, "key.%s.%s", name, is_long ? "long" : "short");
    const char *dflt = "none";
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].code == code) dflt = is_long ? known[i].dlong : known[i].dshort;
    return store_get(k, dflt);
}

void key_action(const char *act) {
    if (!strcmp(act, "none") || !*act) return;
    logmsg("keys: action %s", act);
    if (!strcmp(act, "screen_toggle")) power_toggle();
    else if (!strcmp(act, "screen_off")) power_off("key");
    else if (!strcmp(act, "page_next")) ui_page_step(+1);
    else if (!strcmp(act, "page_prev")) ui_page_step(-1);
    else if (!strcmp(act, "status_page")) ui_page_set(UI_PAGE_STATUS);
    else if (!strcmp(act, "mode_next")) hm_mode_step(+1);
    else if (!strcmp(act, "mode_prev")) hm_mode_step(-1);
    else if (!strcmp(act, "menu")) hm_menu_toggle();
    else if (!strcmp(act, "bright_up")) power_bright_step(+1);
    else if (!strcmp(act, "bright_down")) power_bright_step(-1);
    else if (!strcmp(act, "mode_toggle") || !strcmp(act, "mode_a") || !strcmp(act, "mode_b")) {
        char m[2] = {act[5] == 'a' ? 'A' : act[5] == 'b' ? 'B' : (gadget_mode() == 'A' ? 'B' : 'A'), 0};
        store_set("usb_mode", m);
        gadget_set_mode(m[0]);
        ui_toast(m[0] == 'A' ? "USB MODE A: DISPLAY" : "USB MODE B: SMART");
    } else logmsg("keys: unknown action '%s'", act);
}

/* value: 1 press, 0 release, 2 autorepeat */
void keys_event(int code, int value) {
    power_user_activity();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_KEYS_TRACKED; i++) {
        if (held[i].code == code && held[i].down_ms >= 0) slot = i;
        if (held[i].down_ms < 0 && free_slot < 0) free_slot = i;
    }
    if (value == 1) {
        if (slot < 0) slot = free_slot >= 0 ? free_slot : 0;
        held[slot].code = code;
        held[slot].down_ms = now_ms();
        held[slot].woke = power_screen_off();
        if (held[slot].woke) power_wake("key"); /* first press in the dark only wakes */
        return;
    }
    if (value != 0 || slot < 0) return;
    long dur = now_ms() - held[slot].down_ms;
    int woke = held[slot].woke;
    held[slot].down_ms = -1;
    if (woke) return;
    if (dur < LONG_MS && hm_menu_key(code)) return; /* the open menu navigates with keys */
    if (code == KEY_POWER && dur >= 3000) return; /* reboot already handled by keys_tick */
    key_action(binding(code, dur >= LONG_MS && code != KEY_POWER));
}

/* Call every loop iteration: the power-key 3 s reboot fires while still held. */
int keys_tick(void) {
    int next = 1 << 30;
    for (int i = 0; i < MAX_KEYS_TRACKED; i++) {
        if (held[i].down_ms < 0) continue;
        long dur = now_ms() - held[i].down_ms;
        if (held[i].code == KEY_POWER) {
            if (dur >= 3000) do_reboot("power key held 3 s", NULL);
            next = 3000 - (int)dur < next ? 3000 - (int)dur : next;
        }
    }
    return next;
}

void keys_init(void) {
    for (int i = 0; i < MAX_KEYS_TRACKED; i++) held[i].down_ms = -1;
}

int keys_dump(char *buf, size_t len) {
    int n = 0;
    for (unsigned i = 0; i < sizeof known / sizeof *known && n < (int)len; i++)
        n += snprintf(buf + n, len - n, "%-8s short=%-14s long=%s\n", known[i].name,
                      binding(known[i].code, 0), known[i].code == KEY_POWER ? "(hold 3 s: reboot)" : binding(known[i].code, 1));
    return n;
}
