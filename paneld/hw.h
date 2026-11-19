// The phone hardware layer: everything paneld does differently from one phone to the next.
//
// Each phone has devices/<dev>/hw.c, which defines `phone` starting from HW_DEFAULTS and lists its own values:
//
//     const struct phone_hw phone = { HW_DEFAULTS, .name = "...", .usb = HW_USB_SAMSUNG_COMPOSITE, ... };
//
// Rules (see docs/porting-guide.md, "The hardware layer"):
//   - A port changes how paneld drives its phone here, in its own hw.c, not with checks in shared code.
//   - A new field gets a default in HW_DEFAULTS that is the behaviour every existing phone had before the field
//     existed, so adding a field can't change another phone. Phones that need something else set the field.
//   - No runtime guessing of the phone type ("if this sysfs file exists it must be a Samsung composite kernel"):
//     declare it. Runtime detection stays only where the answer is unambiguous (e.g. mounting devtmpfs fails).
#ifndef PANELD_HW_H
#define PANELD_HW_H

#include <linux/input.h>

enum hw_usb {
    HW_USB_CONFIGFS,          /* plain configfs gadget: link functions, write UDC */
    HW_USB_SAMSUNG_COMPOSITE, /* Samsung android0 layer: functions + enable (Galaxy S8 / Exynos 8895) */
};

struct hw_key {
    int code;          /* Linux key code, 0 = the phone has no such key */
    const char *label; /* on-screen name ("XCOVER") */
};

struct phone_hw {
    const char *name;           /* devices/<name> */
    int fb_swap_rb;             /* the panel shows the framebuffer as BGR although fbdev reports RGB */
    const char *charge_ctl;     /* battery power_supply attribute that switches charging */
    int charge_ctl_invert;      /* 1 when writing 1 stops charging (Samsung batt_slate_mode) */
    struct hw_key mode_key;     /* next mode / hold: menu. Selects in the menu */
    struct hw_key page_key;     /* next page / hold: previous page. Moves in the menu. code 0: hold Vol Up/Down */
    enum hw_usb usb;
    const char *sensor_hub_on;  /* sysfs attribute that powers the sensor co-processor, "" = the kernel does it */
};

/* The behaviour every phone had before each field existed. Never change a value here: add a field instead. */
#define HW_DEFAULTS                                                                                                   \
    .name = "?", .fb_swap_rb = 0, .charge_ctl = "charging_enabled", .charge_ctl_invert = 0,                          \
    .mode_key = {0xfc, "XCOVER"}, .page_key = {KEY_MACRO17, "TOP"}, .usb = HW_USB_CONFIGFS,               \
    .sensor_hub_on = "/sys/devices/platform/11980000.contexthub/poweron"

extern const struct phone_hw phone;

/* paneld/hwinfo.c: the on-screen key hints, and the behaviour print CI compares (PANELD_PRINT_CONFIG=1) */
#include <stddef.h>
void hw_hint_mode(char *b, size_t len);
void hw_hint_page(char *b, size_t len);
void hw_hint_trackpad(char *b, size_t len);
void hw_print_config(void);

static inline int hw_is_mode_key(int code) { return code && code == phone.mode_key.code; }
static inline int hw_is_page_key(int code) { return code && code == phone.page_key.code; }

#endif
