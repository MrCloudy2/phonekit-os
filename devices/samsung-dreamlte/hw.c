// How paneld drives the Samsung Galaxy S8 (dreamlte, Exynos 8895). Fields: paneld/hw.h.
#include "hw.h"

const struct phone_hw phone = {
    HW_DEFAULTS,
    .name = "samsung-dreamlte",
    /* DECON shows BGR, as on the 9611. First boot, 2026-11-18: test-pattern bar 2 (yellow) showed light blue */
    .fb_swap_rb = 1,
    /* No battery/charging_enabled on this kernel. batt_slate_mode=1 stops charging (TWRP 2026-11-18: Discharging,
       0 resumes Fast). The phone then runs from the battery, so the limiter cycles 60 -> 40 -> 60 % */
    .charge_ctl = "batt_slate_mode",
    .charge_ctl_invert = 1,
    .mode_key = {0x2bf, "BIXBY"},
    .page_key = {0, ""},                /* no page key: hold Vol Up/Down for pages */
    .usb = HW_USB_SAMSUNG_COMPOSITE,    /* functions join via android0/functions, enable binds */
    .sensor_hub_on = "",                /* SSP hub is its own MCU, up from boot */
};
