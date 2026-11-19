// How paneld drives <your phone>. Every field and its meaning: paneld/hw.h. Start from HW_DEFAULTS and set
// what differs; when you're done, list every field explicitly (as the other devices do), so a later change
// to a default can't move your phone.
#include "hw.h"

const struct phone_hw phone = {
    HW_DEFAULTS,
    .name = "vendor-model",
    .fb_swap_rb = 0,                    /* 1 if `ppctl colortest` shows red as blue */
    .charge_ctl = "charging_enabled",   /* ls /sys/class/power_supply/battery/ */
    .charge_ctl_invert = 0,
    .mode_key = {0, ""},                /* a spare side key, if any: getevent -l, then {code, "LABEL"} */
    .page_key = {0, ""},
    .usb = HW_USB_CONFIGFS,             /* HW_USB_SAMSUNG_COMPOSITE if UDC alone gives -EINVAL (see porting guide) */
    .sensor_hub_on = "",                /* sysfs attribute that powers a sensor co-processor, if the phone has one */
    .usb_config_name = "phonepanel",    /* "Conf 1" on Exynos 9810 composite kernels (see socs/exynos9810/quirks.md) */
    .bt = HW_BT_SCSC,                   /* HW_BT_BCM_UART + .bt_uart/.bt_rfkill/.bt_fw for a Broadcom UART chip */
};
