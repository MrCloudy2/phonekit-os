# Exynos 8895 (universal8895)

Devices: [Samsung Galaxy S8](../../devices/samsung-dreamlte/device.yaml) (SM-G950F, port in progress).
Other phones on this SoC: Galaxy S8+ (SM-G955F, `dream2lte`), Note 8 (SM-N950F, `greatlte`). The S8 and S8+
share one kernel and differ in the DTB table.

Nothing here has been tested under PhoneKit yet. These notes come from the TWRP 3.7.0 dreamlte image
(kernel 4.4.111) and should be checked on the first boot.

## Boot image (Samsung, Android 7-9 era)
- Header v0 with Samsung's extension: the word at offset 40 (AOSP's `header_version`) is the size of a DTB
  table (`DTBH` magic), stored after the second stage. AOSP `unpack_bootimg` fails on it.
  `scripts/samsung-bootimg.py` unpacks and repacks it; `make import` and `mkimage.sh` use it automatically.
- No AVB footer (`AVB_FOOTER := 0`). Page size 2048, kernel at 0x10008000, ramdisk at 0x11000000.
- Partition names are upper case (`RECOVERY`, `BOOT`). Stock Android 9 has `/dev/block/by-name/`; TWRP's fstab
  uses `/dev/block/platform/11120000.ufs/by-name/` (`scripts/phone.sh` checks both).

## Display: Samsung DECON (fbdev), expected
- The kernel has the DECON fbdev driver (`decon_fb_*`, `decon_pan_display`), like the 9611.
- **Colour order: BGR, same as the 9611** (verified on the S8, 2026-11-18): set `.fb_swap_rb = 1` in `hw.c`.
- `/dev/fb0` 32 bpp, stride 5760, virtual 1440×5920 (double buffer via pan). Backlight `/sys/class/backlight/panel`,
  **max 36600** (the 9611 XCover Pro: 306). paneld scales its brightness to `max_brightness`, so nothing to configure.
- Black-after-wake quirk: still to check (blank/unblank).
- Panel S6E3HA6 (OLED, 1440×2960). Black pixels cost no power on OLED; mostly dark screens will save battery.

## USB: DWC3 (`10c00000.dwc3`) behind Samsung's composite layer
- **Samsung composite gadget** (`CONFIG_USB_CONFIGFS_UEVENT` + `CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE`):
  - Linking a function in configfs only parks it (`gi->linked_func`). It joins the config when its name is
    written to `/sys/class/android_usb/android0/functions` (comma list of `f->name`: `acm`, `rndis`, `ncm`,
    `mtp`; FunctionFS functions are **all** named `adb`, whatever the instance: `f_fs.c` hard-codes
    `func->function.name = "adb"`, so the GUD display (`ffs.gud`) is enabled as `adb` too). `android0/enable` = 1 binds to the
    UDC name hard-coded in the kernel (`10c00000.dwc3`) and connects.
  - Writing `UDC` on its own binds an empty config: `configfs-gadget 10c00000.dwc3: failed to start <g>: -22`,
    then `Config c/1 of <g> needs at least one function.` Samsung's own init (and TWRP's `init.recovery.usb.rc`)
    does `enable 0` → `functions` → `UDC` → `enable 1`. paneld detects `android0/functions` and does the same.
  - After `enable 0` the functions are parked again; unlinking one then WARNs `Unable to locate function to
    unbind` and leaks it. Unlink while still bound (that forces an unbind), then `enable 0`.
  - Every rebind (`enable 0` → `1`, i.e. every USB mode switch) re-creates the function devices: with devtmpfs,
    `/dev/hidgN` is deleted and made again, so an fd opened before the switch points at a deleted node and writes
    just return `EAGAIN`. paneld reopens its HID nodes after each bind (`gadget_bind_gen()`).
  - Only one configfs gadget can exist: a second `mkdir` WARNs `cannot create duplicate filename
    '/devices/virtual/android_usb/android0'`.
  - Source: `drivers/usb/gadget/configfs.c` (`make_usb_func_link`, `functions_store`, `enable_store`) in the
    universal8895 trees, e.g. github.com/acroreiser/android_kernel_samsung_universal8895.
- configfs, FunctionFS, RNDIS and NCM are in the TWRP kernel. **No HID gadget** (`f_hid`) and no mass storage in
  the TWRP or /e/OS kernels: build from source (`8890q/android_kernel_samsung_universal8895`, `lineage-21`) with
  `kernel/phonepanel.cfg`. On the source kernel HID (MOUSE/KEYBOARD/COMBO/SCREEN touch) and GUD (SCREEN) work;
  on the TWRP 4.4.111 kernel GUD frames never reached the panel.

## Kernel from source (4.4.302, host clang 23)
- The Makefile gives clang `--prefix=<binutils dir>/` and turns off the integrated assembler; clang 23 then runs the
  host's x86 `as` (`unrecognized option '-EL'`). Pass `CLANG_PREFIX=--prefix=.../aarch64-linux-gnu-`.
- Empty directories are linked with `$(LD) -r` and no objects, which ld.lld rejects: link with GNU ld.
- Samsung code is full of `int f()` declarations (an error with `-Werror` on clang 23): `-Wno-strict-prototypes`.
- clang 23 found two real backport bugs: `seq_put_decimal_ull()` called with a string in the pidfd code, and
  `cpuset_allow_attach()` missing its `css` argument. Patches in `devices/samsung-dreamlte/kernel-patches/`.
- All of this is in `devices/samsung-dreamlte/device.mk` (`KERNEL_MAKE_FLAGS`).

## Power: MAX77865 + Samsung battery driver
- `charging_enabled` and `batt_slate_mode` exist in the kernel, so paneld's charge limiter (`paneld/power.c`)
  may work unchanged. Unverified.

## Sensors: SSP on the Broadcom GPS chip (no auto-rotate without Android)
- The sensor hub firmware runs on the **BCM4775 GPS chip** (`SSPBBD`), not on a contexthub or its own MCU. The
  kernel registers `/dev/bbd_sensor`, `/dev/bbd_control` and `/dev/bbd_patch`; Android's Broadcom location daemon
  patches the chip's firmware and bridges sensor packets. Without it: `[SSPBBD]: ssp_down == true`, every SSP
  command fails (`CMD Fail -1`), and the IIO devices (`accelerometer_sensor`, …) exist but deliver nothing.
- So PhoneKit has no orientation sensor on this SoC/board for now: rotation is manual (Settings, `ppctl rotate`).
  paneld finds no contexthub and skips the power-on step (`paneld/sensors.c`).
- This SSP driver's `enable` is a decimal bitmask (bit 0 = accelerometer) with per-sensor `*_poll_delay` (ns),
  unlike the 9611's `type*10+1`. Only matters if the hub can be brought up one day.
