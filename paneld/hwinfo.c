// What paneld does on this phone, as text: the on-screen key hints, and `PANELD_PRINT_CONFIG=1 paneld`, which prints
// everything paneld derives from devices/<dev>/hw.c and the shared defaults. CI compares that with
// devices/<dev>/paneld-config.txt, so a shared change that alters any phone's behaviour shows up in the PR diff.
#include "paneld.h"

#include <stdio.h>

void hw_hint_mode(char *b, size_t len) { snprintf(b, len, "%s: NEXT MODE  HOLD: MENU", phone.mode_key.label); }

void hw_hint_page(char *b, size_t len) {
    if (phone.page_key.code) snprintf(b, len, "%s: PAGES  VOL: BRIGHTNESS", phone.page_key.label);
    else snprintf(b, len, "VOL: BRIGHTNESS  HOLD VOL: PAGES");
}

void hw_hint_trackpad(char *b, size_t len) {
    snprintf(b, len, "%s: NEXT MODE   HOLD %s: MENU", phone.mode_key.label, phone.mode_key.label);
}

void hw_print_config(void) {
    static const char *const usb[] = {"configfs (UDC)", "samsung-composite (android0 functions + enable)"};
    char b[2048];
    printf("# paneld behaviour on this phone: PANELD_PRINT_CONFIG=1 paneld (devices/<dev>/hw.c + shared defaults)\n");
    printf("device %s\n", phone.name);
    printf("fb_swap_rb %d\n", phone.fb_swap_rb);
    printf("charging /sys/class/power_supply/battery/%s, write %s to charge\n", phone.charge_ctl,
           phone.charge_ctl_invert ? "0" : "1");
    printf("mode_key %d %s\n", phone.mode_key.code, phone.mode_key.label);
    if (phone.page_key.code) printf("page_key %d %s\n", phone.page_key.code, phone.page_key.label);
    else printf("page_key (none: hold Vol Up/Down for pages)\n");
    printf("usb %s\n", usb[phone.usb]);
    printf("sensor_hub_on %s\n", phone.sensor_hub_on[0] ? phone.sensor_hub_on : "(none: the kernel powers the hub)");
    hw_hint_mode(b, sizeof b); printf("hint_status_1 %s\n", b);
    hw_hint_page(b, sizeof b); printf("hint_status_2 %s\n", b);
    hw_hint_trackpad(b, sizeof b); printf("hint_trackpad %s\n", b);
    printf("# default key bindings\n");
    keys_dump(b, sizeof b);
    fputs(b, stdout);
}
