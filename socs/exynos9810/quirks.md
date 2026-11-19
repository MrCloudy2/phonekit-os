# Exynos 9810 (universal9810)

Devices: [Samsung Galaxy S9+](../../devices/samsung-star2lte/device.yaml) (SM-G965F, port in progress).
Other phones on this SoC: Galaxy S9 (SM-G960F, `starlte`), Note 9 (SM-N960F, `crownlte`). All three share one
kernel tree with their own defconfig and DTBs.

Verified on a Galaxy S9+ under PhoneKit on 2026-11-19 (LineageOS 24.0 kernel built from source; pptest 91/1/1 at
first, then 92 passes after two test fixes), plus the ROM kernels' `/proc/config.gz`, their boot images and the kernel
sources named below. Most of it matches the [Exynos 8895](../exynos8895/quirks.md), so read that too.

## Boot image: same Samsung layout as the 8895
- Header v0 with Samsung's DT table (`DTBH`) size at offset 40, the table after the second stage, and a
  `SEANDROIDENFORCE` trailer. `scripts/samsung-bootimg.py` reads it, and the official TWRP 3.7.0 `starlte` image
  round-trips byte for byte.
- LineageOS's exynos9810-common tree says `BOARD_BOOT_HEADER_VERSION := 1`, but the images it produces (and TWRP's)
  still carry the DT size at offset 40, so treat them as the 8895 layout.
- The phone has `DTB` and `DTBO` partitions (8 MiB each); on the S9+ test phone both are all zeros. The DT table
  is in boot/recovery.
- No AVB footer. Page size 2048, kernel at 0x10008000, ramdisk at 0x11000000. Partition names are upper case.

## Kernel sources
- LineageOS's official tree ends at lineage-20: `LineageOS/android_kernel_samsung_exynos9810` (4.9.118).
- Current unofficial LineageOS (22.2 to 24.0, krazey90 on XDA) builds come from
  `ExyHyperBrick/android_kernel_samsung_exynos9810` (4.9.337, `LOCALVERSION -ies`). LineageOS 24.0-20261112 runs
  commit `401189fdfc65` (branch `lineage-24.0`); 23.2-20260614 ran `87ff302a9ad9`.
- That tree builds with host clang 23 after three new diagnostics are made warnings again
  (`-Wdefault-const-init-unsafe`, `-Wunused-but-set-global`, `-Wframe-larger-than`): `KERNEL_KCFLAGS` in the
  device's `device.mk`. Its config then equals the ROM kernel's `/proc/config.gz` plus the PhoneKit fragment.
- The ROM kernel has **no devtmpfs, no HID gadget and no mass storage gadget**: build from source with the device's
  `kernel/phonepanel.cfg` for MOUSE/KEYBOARD/SCREEN touch.

## USB: Samsung composite, plus a "GSI" trap the 8895 doesn't have
- DWC3 at `10c00000.dwc3` behind Samsung's composite layer, as on the 8895: `hw.c` `.usb = HW_USB_SAMSUNG_COMPOSITE`.
- **The config must be named `Conf 1`.** `drivers/usb/gadget/configfs.c` `is_symboliclink_change_mode()` treats any
  other `configs/<c>/strings/0x409/configuration` as a GSI/VTS image: links then go straight into the config while
  nothing is parked, and once something is, every new link runs `make_adb_connection_for_gsi()` ("ADB Only mode for
  recovery or VTS test") and clears the config (`Clear cfg->func_list`). Under PhoneKit's old name (`phonepanel`) the
  boot-time mode B worked, but the first mode switch left only the three ACM ports: no HID nodes, no RNDIS, no adb.
  Fix: `hw.c` `.usb_config_name = "Conf 1"` (field added for this; the default stays `phonepanel`).
- Mode switches, HID (MOUSE/KEYBOARD/COMBO/SCREEN touch), GUD, RNDIS and adb all pass after that (test-usb 11/11,
  test-input-modes 16/16, test-panel 21/21, test-phonebox 9/9).

## CPU frequency: the hotplug governor caps the big cluster
- `HP_GOV` allows the big cluster's 2314 MHz only with one or two big cores online (modes 1-2); with all four (modes
  0, 3, 4) the max is 1794 MHz. Writing 2314000 to `policy4/scaling_max_freq` reads back 1794000. paneld's cap and
  restore still work; test-power compares the max after wake with the max before screen-off.

## Display, power, sensors
- Display: DECON fbdev (`CONFIG_EXYNOS_DECON_9810`), 32 bpp, 1440x2960, double buffer via pan; test-display 5/5.
  LineageOS's recovery uses `ABGR_8888`, so BGR is expected (`fb_swap_rb = 1`; colours by eye still to confirm).
  Backlight `/sys/class/backlight/panel`, max 44800 on the S9+.
- Charging: Samsung battery driver with MAX77705; `battery/batt_slate_mode` (no `battery/charging_enabled`) stops
  charging, as on the 8895: test-power's limiter checks pass. Power on the S9+: about 800 mW active (test pattern,
  backlight 128), 430 mW dim, 130-150 mW off.
- Sensors: `CONFIG_SENSORS_SSP_BBD`, the SSP hub on the Broadcom GPS chip, as on the 8895. Expect manual rotation
  only (manual rotation passes test-touch in all four orientations; auto-rotate untested).
- The ROM kernel has no devtmpfs; the source build keeps it off too (paneld creates its nodes). There is no
  `/dev/block` either: to read a partition from PhoneKit, `mknod` it from `/sys/class/block/<name>/dev`.
- The bootloader ignores the boot image's cmdline: options such as `initcall_debug` must be built in
  (`CONFIG_CMDLINE_EXTEND`). Kernel to paneld takes ~3.2 s on the S9+ (S8: 1.4 s), spread over many probes.

## Wi-Fi: Broadcom BCM4361 B2 over PCIe (bcmdhd 101.16)
- The driver asks the kernel's firmware loader (`request_firmware`, `DHD_LINUX_STD_FW_API`) for bare names
  (`bcmdhd_sta.bin_b2`, `nvram.txt_r02a_b2`, `bcmdhd_clm.blob`), so the files go to `/lib/firmware`: put them in
  `devices/<dev>/prebuilt/firmware/lib/firmware/` (from the ROM's `/vendor/firmware`). The 8895's driver read
  `/vendor/etc/wifi` directly.
- It also asks for `logstrs.bin`, which Samsung doesn't ship. With `CONFIG_FW_LOADER_USER_HELPER_FALLBACK` every
  power-up then waits 25 s for a userspace helper, and paneld's scan timeout killed `ifconfig` in the middle
  (`request_irq failed with -4`). The device's kernel fragment turns the fallback off.
- The nvram is chosen from the module's OTP (Semco `91 33` -> `r02a`); the MAC comes from the chip's OTP.
- WPA2-PSK, DHCP, DNS and tethering work; ~12 mW when connected and idle.

## Bluetooth: Broadcom BCM4361 B2 on a UART
- `ttySAC1` (`10840000.uart`), power through `bcm43xx.c`'s rfkill `bcm43xx Bluetooth`, patchram
  `bcm4361B2_semco.hcd` / `_murata.hcd` from `/vendor/firmware` (the Wi-Fi nvram tells which module you have).
- paneld does libbt-vendor's bring-up itself (`paneld/bt_bcm.c`, `hw.c` `.bt = HW_BT_BCM_UART`): reset at 115200,
  3 Mbaud, patchram, reset, 3 Mbaud, Write BD_ADDR from `/efs/bluetooth/bt_addr` (a copy in the image, from
  `prebuilt/firmware/efs-root/`), then the vhci bridge the XCover Pro uses (`CONFIG_BT_HCIVHCI`). Up in ~0.8 s.
- Without the address file the chip uses Broadcom's shared default `43:61:B2:00:1F:AC`.
- The 8895 (Galaxy S8) has a BCM4361 as well; the same path should work there with its own tty and `.hcd`.
