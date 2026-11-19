# Samsung Galaxy S10e (SM-G970F, Exynos 9820): port verification

Status: 🟨 2026-11-19. Fourth PhoneKit device and first Exynos 9820, ported in one session from stock Android 12.
On the official LineageOS 23.2 kernel built from source: **pptest 91 pass, 1 fail, 1 skip** (the fail: screen-off power is 1/5 of active only to within 1 mW). Checked by hand: colours and touch by
eye, Wi-Fi scan, Bluetooth controller bring-up. Open: a real Wi-Fi join (needs the owner's network), Bluetooth
pairing, auto-rotate by hand, tethering, DECK/RESCUE, a long soak.
Facts with sources: `devices/samsung-beyond0lte/device.yaml`; chip-wide findings: `socs/exynos9820/quirks.md`.
Latest `scripts/pptest.py` report: `samsung-beyond0lte-report.md`.

## How it was done
| Step | What happened |
|---|---|
| Stage 0 | SM-G970F/DS on stock Android 12 (bootloader `G970FXXSGHWC2`) with OEM unlock and USB debugging on. Official LineageOS supports it (`beyond0lte`, 23.2, kernel `LineageOS/android_kernel_samsung_exynos9820`). Stock adb never showed the authorization prompt, so the port went straight to download mode; the owner approved wiping the phone |
| Download mode | `heimdall download-pit` (read-only) gave the PIT: RECOVERY = 16512 x 4 KiB = 67633152 B, the size of LineageOS's recovery.img. The next heimdall command failed (`Protocol initialisation failed!`): one session per download-mode boot |
| ROM | `heimdall flash --RECOVERY` with the official lineage-23.2-20261114 recovery (sha256 checked against download.lineageos.org), forced reset into recovery, Format data, `adb sideload` of the signed ROM zip. LineageOS 23.2 userdebug, adb root through Rooted debugging |
| Backups | recovery, boot, dtb, dtbo, vbmeta dumped with adb root plus sha256; recovery and boot are byte-identical to the official images; PIT kept |
| Boot image | Plain AOSP header v1 with a recovery DTBO and an AVB footer: `make import` (unpack_bootimg) handled it unchanged; `AVB_FOOTER := 1`. vbmeta has `--flags 3`, so PhoneKit's `--algorithm NONE` footer boots |
| First boot | Prebuilt LineageOS kernel: paneld ready 2.8 s after the kernel, test page on screen, but nothing on USB: `write android0/functions=acm,rndis,adb: No such file or directory` (see below) |
| USB fix | The 9820's `android0` has no `functions`/`enable` (they need `CONFIG_USB_OLD_CONFIGFS`, off here); LineageOS binds its gadget through `UDC`. `hw.c` `.usb = HW_USB_CONFIGFS` and the second boot enumerated (3 ACM + RNDIS + adb) |
| Kernel source | The running commit `a412cfaf86b9` is on LineageOS's `lineage-23.2` branch. Built with host clang 23 after `KERNEL_KCFLAGS` (as on the 9810), the 9810's `mkcompile_h` fix re-made for this tree, and a new `extract-cert` patch for OpenSSL 4 (no ENGINE API). Its config equals the ROM's `/proc/config.gz` plus the fragment (HID, mass storage, VHCI, no FW user-helper fallback) |
| Wi-Fi/BT firmware | Copied from the ROM's `/vendor/firmware` into `prebuilt/firmware/lib/firmware`, plus the phone's `/mnt/vendor/efs/bluetooth/bt_addr` and `wifi/.mac.info` into `prebuilt/firmware/efs-root` (gitignored) |

## What needed changes
Nothing outside `devices/samsung-beyond0lte/` and the docs: every difference fit the existing hardware layer. The
other phones' `paneld-config.txt` and `footprint.txt` are unchanged.

| Problem on the S10e | Fix |
|---|---|
| Samsung composite kernel without `android0/functions` | `hw.c` `.usb = HW_USB_CONFIGFS` (as the XCover Pro); default config name (the `Conf 1` trap is compiled out with `CONFIG_USB_OLD_CONFIGFS`) |
| Kernel host tool `extract-cert` doesn't link with OpenSSL 4 | `kernel-patches/0002`: skip the PKCS#11 branch under `OPENSSL_NO_ENGINE` |
| `mkcompile_h` line differs from the 9810 tree | `kernel-patches/0001`, same fix rewritten for this file |
| `battery/charging_enabled` exists (the S8/S9+ needed `batt_slate_mode`) | `hw.c` `charge_ctl = "charging_enabled"`, not inverted (checked under LineageOS: 0 -> Discharging, 1 -> Charging) |

## Results (2026-11-19, source kernel `4.14.356-openela-rc1-ga412cfaf86b9`)
| Area | Result |
|---|---|
| pptest | **91 pass, 1 fail, 1 skip**: display 5/5, touch 10/10, power 11/12, keys 8/8 (+1 skip: no top key), usb 11/11, input-modes 16/16, panel 21/21, phonebox 9/9. Three full runs, same result (the last at `3bd2414`: off 144 mW vs 709 active) |
| Power | 712 mW active (test pattern, brightness 128), ~405 mW dim, 143-156 mW off (panel blanked, all three CPU clusters capped). "off < 1/5 of active" fails by 1-14 mW: the 5.8" panel draws less when active than the S9+'s, while the off floor is the same (S9+ 125-150 mW). A real measurement, not loosened for one phone (the S8 fails the same check at 245 mW) |
| Charge limiter | `charging_enabled`; above the window the phone runs from the battery (~8 mA), below it charges (~160 mA) |
| Boot | kernel -> paneld ready 1.9 s (prebuilt and source kernel alike) |
| USB | mode B: 3 ACM + RNDIS + adb (`18d1:4ee4`); `hid yes` on the source kernel; test-usb, test-input-modes pass |
| Colours, touch | by eye (owner): red border on all four edges, bar 2 yellow with `fb_swap_rb = 1`; strokes follow the finger |

## Features checked by hand (2026-11-19)
| Feature | Result |
|---|---|
| Wi-Fi (M10) | BCM4375 B1, Semco module: `bcmdhd_sta.bin_b1` + `nvram.txt_CS01_semco_b1` through `request_firmware`, firmware up with the phone's own MAC, scan finds the networks around. Join not tried (needs the owner's network) |
| Bluetooth (M10c) | `HW_BT_BCM_UART`: Semco patchram `bcm4375B1_semco.hcd` (1260 records), controller up at 3 Mbaud in 1.9 s with the phone's own address, hci0, inquiry completes. LineageOS's libbt loads `bcm4375B1_murata.hcd` instead (first file matching the chip name); the Semco file matches the module the Wi-Fi side reports. Pairing not tried |
| Sensors | paneld finds the SSP accelerometer as an IIO device and starts autorotate (on the S8/S9+ the hub stays down without `lhd`). Not yet checked by turning the phone |
