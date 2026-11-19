# Exynos 9611 (universal9611)

Devices: [Samsung Galaxy XCover Pro](../../devices/samsung-xcoverpro/device.yaml) (SM-G715FN).
Other phones on this SoC that should share most of this: Galaxy A50/A50s, A51, M21, M31, M30s, Tab S6 Lite.
Unverified until someone ports one; please add yours.

## Display: Samsung DECON (fbdev)
- `/dev/fb0`, ARGB8888 reported, double buffer via pan (virtual height = 2× panel).
- **Colour order**: `decon_pan_display` hard-codes ABGR8888, so the panel shows the buffer as BGR. Set
  `FB_SWAP_RB := 1` in `device.mk`. Check by eye with `scripts/ppctl.py colortest`.
- **Black after wake**: after `FBIOBLANK` powerdown, the window registers reset, and pan never re-enables it
  (`decon_set_par` is `#if 0`'d inside `decon_pan_display`). paneld forces `FBIOPUT_VSCREENINFO` after unblank
  (`fb_restore_window` in `paneld/fb.c`). Already generic; nothing to configure.
- Partial update (`win_update`) is present; paneld uses damage tracking and full-buffer pans.

## USB: DWC3 (`13200000.dwc3`)
- configfs gadget works: ACM, FunctionFS (GUD), HID (needs `CONFIG_USB_CONFIGFS_F_HID=y`, not in the stock
  LineageOS config), RNDIS, mass storage (`CONFIG_USB_CONFIGFS_MASS_STORAGE=y`).
- **NCM enumerates but never transmits** on the 4.14 Samsung kernel; use RNDIS (paneld does).
- FunctionFS: don't keep a blocking ep0 read pending across a UDC unbind (deadlock); paneld polls ep0.
- f_hid: don't close and reopen `/dev/hidgN` around a rebind. It locked up the 4.14 kernel hard (MOUSE mode, 2026-11-19);
  the nodes survive a rebind here, so paneld keeps them open (the reopen is only for Samsung composite kernels).
- High-speed only (USB 2.0), so SCREEN mode tops out around 50 fps at 1080×2340 with LZ4.
- Rare: a soft replug (`soft_connect`) can leave the DWC3 stuck in state `default`; the host then logs
  `device not accepting address, error -71`, a physical replug doesn't help, a reboot does (seen once in ~150).

## Sensors: CHUB sensor hub (SSP)
- Firmware is built into the kernel (`firmware/sensorhub/`). Power on:
  `echo 1 > /sys/devices/platform/11980000.contexthub/poweron` (address may differ per board; check DT).
- Enable a sensor: `echo $((type*10+1)) > /sys/class/sensors/ssp_sensor/enable`, then the IIO device's
  `scan_elements/in_timestamp_en=1` and `buffer/enable=1`. Samples = payload + 8-byte timestamp.
- `device_orientation` = type 27 (one event per change, computed on the hub). Accelerometer = type 1.
- Hard-coded in `paneld/sensors.c` today; move to a define when a second device differs.

## Power (Samsung battery driver, SM5713 PMIC on the XCover Pro)
- Charge control: `/sys/class/power_supply/battery/charging_enabled` (0/1). The driver may re-enable it on
  cable events; paneld re-asserts every 30 s.
- Power measurement needs `batt_slate_mode` so the fuel gauge reads the system draw.
- Expect `sec_bat_get_battery_info: Fail to set psy` spam without Android; harmless.

## Kernel (Samsung 4.14 trees, e.g. LineageOS universal9611)
- Builds with a modern host clang after two patches: OpenSSL 3+/4 dropped the ENGINE API (`extract-cert`),
  and `mkcompile_h` breaks on multi-line compiler banners. See
  `devices/samsung-xcoverpro/kernel-patches/`.
- **Don't disable `CONFIG_ESE_SECURE` / Samsung NFC**: boot hangs before the display (secure world owns the SPI bus).
- The audio codec (`RT5665`) probe costs ~390 ms of boot; safe to disable.
- `PROBE_PREFER_ASYNCHRONOUS` gains nothing (the kernel waits for async probes before `/init`).
- The kernel services the hardware watchdog itself.

## Boot (Samsung)
- boot.img header v2, page size 2048, gzip ramdisk; the bootloader ignores the boot.img cmdline (uses `CONFIG_CMDLINE`).
- Unlocked bootloader accepts an AVB footer with algorithm NONE (orange state warning).
- Download mode + heimdall for recovery. Forced reset: Vol Down + Power ~10 s.
- LineageOS restores recovery on boot unless `persist.vendor.recovery_update=false`.
