// How paneld drives the Samsung Galaxy XCover Pro (Exynos 9611). Fields: paneld/hw.h.
#include "hw.h"

const struct phone_hw phone = {
    HW_DEFAULTS,
    .name = "samsung-xcoverpro",
    .fb_swap_rb = 1,                    /* DECON shows BGR; verified by eye with `ppctl colortest` */
    .charge_ctl = "charging_enabled",
    .charge_ctl_invert = 0,
    .mode_key = {0xfc, "XCOVER"},       /* the programmable side key */
    .page_key = {KEY_MACRO17, "TOP"},   /* the top (PTT) key */
    .usb = HW_USB_CONFIGFS,             /* android0 exists, but plain configfs + UDC works (4.14) */
    .sensor_hub_on = "/sys/devices/platform/11980000.contexthub/poweron", /* CHUB co-processor */
};
