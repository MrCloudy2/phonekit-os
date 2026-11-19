// How paneld drives the Samsung Galaxy S10e (beyond0lte, Exynos 9820). Fields: paneld/hw.h.
#include "hw.h"

const struct phone_hw phone = {
    HW_DEFAULTS,
    .name = "samsung-beyond0lte",
    /* BGR, as LineageOS's TARGET_RECOVERY_PIXEL_FORMAT ABGR_8888 says: test-pattern bar 2 yellow, red border
       (owner, by eye, 2026-11-19) */
    .fb_swap_rb = 1,
    /* battery/charging_enabled exists here (unlike the S8/S9+): 0 -> Discharging at ~-3 mA, 1 -> Charging again
       (LineageOS 23.2, 2026-11-19). batt_slate_mode works too but is not needed */
    .charge_ctl = "charging_enabled",
    .charge_ctl_invert = 0,
    .mode_key = {0x2bf, "BIXBY"},       /* 0x2bf (KEY_WINK) in gpio_keys, getevent -pl */
    .page_key = {0, ""},                /* no page key: hold Vol Up/Down for pages */
    /* CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE is set, but android0 has no functions/enable files: they need
       CONFIG_USB_OLD_CONFIGFS, which the 9820 kernel doesn't set. LineageOS binds g1 through UDC, as on the XCover.
       With HW_USB_SAMSUNG_COMPOSITE the first boot logged "write android0/functions=acm,rndis,adb: No such file" */
    .usb = HW_USB_CONFIGFS,
    .sensor_hub_on = "",                /* SSP hub on the GPS chip (CONFIG_SENSORS_SSP_BBD), no contexthub */
    /* configfs.c has the 9810's "Conf 1" GSI trap (is_symboliclink_change_mode), but only under
       CONFIG_USB_OLD_CONFIGFS, which is off here: the default name is fine */
    .usb_config_name = "phonepanel",
    /* Broadcom BCM4375 rev B1 on UART ttySAC1 (owned by bluetooth under the ROM), powered by bcm43xx.c's rfkill.
       The Wi-Fi side reports a Semco module (nvram.txt_CS01_semco_b1), so the Semco patchram; LineageOS's libbt
       loads bcm4375B1_murata.hcd only because it takes the first file that matches the chip name */
    .bt = HW_BT_BCM_UART,
    .bt_uart = "ttySAC1",
    .bt_rfkill = "bcm43xx Bluetooth",
    .bt_fw = "/lib/firmware/bcm4375B1_semco.hcd",
};
