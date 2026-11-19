# Exynos 9820 (universal9820)

Devices: [Samsung Galaxy S10e](../../devices/samsung-beyond0lte/device.yaml) (SM-G970F, port in progress).
Other phones on this SoC: Galaxy S10 (SM-G973F, `beyond1lte`), S10+ (SM-G975F, `beyond2lte`), S10 5G (SM-G977B,
`beyondx`). LineageOS builds all four from one kernel tree with their own defconfig; the Note 10 family (Exynos 9825)
shares the common device tree.

Verified on a Galaxy S10e under PhoneKit on 2026-11-19 (official LineageOS 23.2 ROM; PhoneKit first on the ROM's own
kernel, then the same commit built from source), plus the ROM kernel's `/proc/config.gz`, the official LineageOS
images and the kernel source named below. It is closer to the [Exynos 9611](../exynos9611/quirks.md) (XCover Pro)
than to the [Exynos 9810](../exynos9810/quirks.md), although the drivers come from the same Samsung family.

## Boot image: plain AOSP header v1, AVB
- Launched on Android 9 with AVB. LineageOS's recovery is a normal header v1 image with a recovery DTBO (about
  1.2 MB) and an AVB hash footer; `unpack_bootimg` reads it, so `make import` needs no Samsung packer (unlike the
  8895/9810 DT-table layout). Page size 2048, kernel at 0x10008000, ramdisk at 0x11000000, tags at 0x10000100.
- The DTB has its own partition (`dtb`, 8 MiB); the boot/recovery images carry no DTB section.
- LineageOS builds `vbmeta` with `--flags 3` (verification disabled) and signs recovery as a chained partition with
  AVB's test key. With the bootloader unlocked (verified boot state orange) a PhoneKit image with an
  `--algorithm NONE` hash footer (`AVB_FOOTER := 1`) boots; the bootloader log says `vbmeta: VERIFICATION_DISABLED
  bit is set`.
- `/dev/block/by-name/` names are **lower case** here (`recovery` -> sda15); heimdall's PIT names are upper case
  (`RECOVERY`). The S8/S9+ have upper-case by-name links.
- Download mode takes one heimdall session per boot: after `heimdall download-pit` (or `print-pit`) with
  `--no-reboot`, the next command fails with `Protocol initialisation failed!` until download mode is entered again.

## Kernel source
- Official LineageOS tree: `LineageOS/android_kernel_samsung_exynos9820` (4.14.356, "openela"). LineageOS
  23.2-20261114 runs commit `a412cfaf86b9` (branch `lineage-23.2`). Defconfigs `exynos9820-<device>_defconfig`.
- It builds with host clang 23 + ld.lld after the same three diagnostics as the 9810 are made warnings again
  (`KERNEL_KCFLAGS`), plus two host-tool patches in the device's `kernel-patches/`: `mkcompile_h` (first compiler
  version line only) and `extract-cert` (OpenSSL 4 has no ENGINE API; `OPENSSL_NO_ENGINE` now skips the PKCS#11
  branch, as `sign-file.c` in this tree already did). Its config is then the ROM's `/proc/config.gz` plus the
  PhoneKit fragment.
- The ROM kernel has devtmpfs, configfs and functionfs, but **no HID gadget and no mass storage gadget**: build from
  source with the device's `kernel/phonepanel.cfg` for MOUSE/KEYBOARD/SCREEN touch.
- `CONFIG_CMDLINE_EXTEND=y` with a built-in `androidboot.boot_devices=13d60000.ufs androidboot.super_partition=system`.

## USB: plain configfs + UDC, not the android0 `functions` path
- DWC3 at `10c00000.dwc3` (super-speed-plus). `CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE=y` as on the 8895/9810, **but
  `CONFIG_USB_OLD_CONFIGFS` is off**, so `/sys/class/android_usb/android0` has only `state`, `bcdUSB` and
  `terminal_version`: no `functions`, no `enable`. LineageOS binds its `g1` gadget by writing `UDC`. Use
  `hw.c` `.usb = HW_USB_CONFIGFS`. With `HW_USB_SAMSUNG_COMPOSITE` the first boot logged `write
  android0/functions=acm,rndis,adb: No such file or directory` and the PC saw nothing.
- The 9810's `Conf 1` GSI trap (`is_symboliclink_change_mode()` in `configfs.c`) is in this tree too, but only inside
  `#ifdef CONFIG_USB_OLD_CONFIGFS`, so it is compiled out: the default config name works.

## Display, input, power, sensors
- Display: DECON fbdev (`drivers/video/fbdev/exynos/dpu20`, panel S6E3FA7 on the S10e), 32 bpp, double buffer via
  pan. LineageOS's recovery uses `ABGR_8888`, and PhoneKit needs `fb_swap_rb = 1` (confirmed by eye on the S10e).
  Backlight `/sys/class/backlight/panel`, max 48600 on the S10e.
- Touch: `sec_ts` (`sec_touchscreen`, 10 slots, 0-4095 on both axes). Keys: `gpio_keys` with Vol Up/Down, Power and
  Bixby (0x2bf).
- Charging: Samsung battery driver with MAX77705. Unlike the 8895/9810, `battery/charging_enabled` exists and works
  (0 -> Discharging, 1 -> Charging again); `batt_slate_mode` works too. `hw.c` uses `charging_enabled`.
- Sensors: SSP hub on the GPS chip (`CONFIG_SENSORS_SSP_BBD`, `CONFIG_SENSORS_SSP_BEYOND`), as on the 8895/9810, but
  the accelerometer shows up as an IIO device under PhoneKit (`sensors: hub up (accel /dev/iio:device1)`).
- Wi-Fi: Broadcom BCM4375 over PCIe (`bcmdhd_101_16`, built in), `DHD_LINUX_STD_FW_API` (firmware through
  `request_firmware()`) and `USE_CID_CHECK` (module-specific nvram), as on the 9810.
- Bluetooth: Broadcom BCM4375 on `/dev/ttySAC1` (owned by `bluetooth` under the ROM), rfkill `bcm43xx Bluetooth`,
  patchram `bcm4375B{0,1}_{semco,murata}.hcd` in `/vendor/firmware`; LineageOS's `libbt_vndcfg.txt`: 3 Mbaud.
- CPUs: three clusters, 4x A55 442-1950 MHz (policy0), 2x A75 507-2314 MHz (policy4), 2x Mongoose M4 520-2730 MHz
  (policy6).
