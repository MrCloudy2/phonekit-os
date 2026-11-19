// How paneld drives the Samsung Galaxy S9+ (star2lte, Exynos 9810). Fields: paneld/hw.h.
#include "hw.h"

const struct phone_hw phone = {
    HW_DEFAULTS,
    .name = "samsung-star2lte",
    /* Expected BGR like the 8895/9611 DECON (LineageOS sets TARGET_RECOVERY_PIXEL_FORMAT ABGR_8888). Check at
       first boot: test-pattern bar 2 must be yellow */
    .fb_swap_rb = 1,
    /* battery/ has batt_slate_mode but no charging_enabled (that one is on max77705-fuelgauge), as on the S8.
       batt_slate_mode=1 stops charging. To verify under PhoneKit */
    .charge_ctl = "batt_slate_mode",
    .charge_ctl_invert = 1,
    .mode_key = {0x2bf, "BIXBY"},       /* KEY_WINK in the star2lte DTS gpio_keys */
    .page_key = {0, ""},                /* no page key: hold Vol Up/Down for pages */
    .usb = HW_USB_SAMSUNG_COMPOSITE,    /* CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE in the running kernel */
    .sensor_hub_on = "",                /* SSP hub on the GPS chip (CONFIG_SENSORS_SSP_BBD), no contexthub */
    /* configfs.c is_symboliclink_change_mode(): any other config name puts the composite layer in "GSI / ADB only"
       mode, and the first USB mode switch dropped HID, RNDIS and adb (2026-11-19, pptest usb) */
    .usb_config_name = "Conf 1",
    /* Broadcom BCM4361 Bluetooth on UART 10840000.uart (Samsung's init chowns /dev/ttySAC1 to bluetooth), powered
       by bcm43xx.c's rfkill; patchram from /vendor/firmware (Semco module, as the Wi-Fi nvram r02a) */
    .bt = HW_BT_BCM_UART,
    .bt_uart = "ttySAC1",
    .bt_rfkill = "bcm43xx Bluetooth",
    .bt_fw = "/lib/firmware/bcm4361B2_semco.hcd",
};
