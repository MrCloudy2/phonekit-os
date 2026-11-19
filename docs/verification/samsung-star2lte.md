# Samsung Galaxy S9+ (SM-G965F, Exynos 9810): port verification

Status: 🟨 2026-11-19. Third PhoneKit device, ported in one session. On the LineageOS 24.0 kernel built from source:
**pptest 92 pass, 0 fail, 1 skip** (no top key). Open: colours and touch checked by eye, Wi-Fi, SCREEN seen on a
desktop, auto-rotate, a long soak, boot time (paneld starts 3.2 s after the kernel; the S8 does it in 1.4 s).
Facts with sources: `devices/samsung-star2lte/device.yaml`; chip-wide findings: `socs/exynos9810/quirks.md`.
Latest `scripts/pptest.py` report: `samsung-star2lte-report.md`.

## How it was done
| Step | What happened |
|---|---|
| Stage 0 | SM-G965F (Exynos) found over Wi-Fi adb running unofficial LineageOS 23.2 (krazey90). The ROM spoofs the lock and build-type props, but `adb root` works. Bootloader `G965FXXUHFVG4` |
| Backups | RECOVERY, BOOT, DTB, DTBO dumped with adb root, sha256 checked on the phone and the PC. DTB/DTBO are all zeros |
| Boot image | Same Samsung header v0 + DT table as the S8 (305 152 B `DTBH`, `SEANDROIDENFORCE` trailer); `scripts/samsung-bootimg.py` handles it unchanged |
| Kernel source | The running kernel's commit was found on `ExyHyperBrick/android_kernel_samsung_exynos9810` (23.2: `87ff302a9ad9`; 24.0: `401189fdfc65`). Built with host clang 23 after `KERNEL_KCFLAGS` (new) relaxes three new diagnostics, and a `mkcompile_h` patch. The resulting config equals the ROM's `/proc/config.gz` plus HID and mass storage |
| First boot | Prebuilt LineageOS 23.2 recovery kernel + PhoneKit ramdisk: the stock phonebox agent connected over USB and drew its dashboard on the first try |
| ROM | Upgraded to LineageOS 24.0-20261112 (Android 17) keeping userdata, at the owner's request: 24.0 recovery, ODM turned into `/metadata` by the maintainer's script (read before running), ROM + MindTheGapps sideloaded. The port then moved to the 24.0 kernel |
| USB bug | After the first USB mode switch only the ACM ports were left. Cause: the 9810 composite layer treats any config name but `Conf 1` as a GSI image (`ADB Only mode for recovery or VTS test`). New `hw.c` field `usb_config_name` |

## What needed changes (all through the hardware layer or general test fixes)
| Problem on the S9+ | Fix |
|---|---|
| First mode switch dropped HID, RNDIS and adb | `struct phone_hw` field `usb_config_name` (default `phonepanel`, so the S8 and XCover Pro are unchanged); the S9+ sets `Conf 1` |
| `KERNEL_MAKE_FLAGS` can pass only one `KCFLAGS` flag (split on spaces) | `scripts/build-kernel.sh`: `KERNEL_KCFLAGS`, empty by default |
| test-power: "CPU caps lifted on wake" expected `cpuinfo_max_freq`; the 9810 hotplug governor allows only 1794 of 2314 MHz with four big cores online | the check accepts the max the kernel allowed before screen-off |
| test-power: "off < 1/5 of active" depended on what the OLED showed (dark PHONEBOX page: active ≈ dim) | the suite draws the test pattern before measuring |
| Clang lists "Found CUDA installation … version" in the kernel version string | `kernel-patches/0001` (same fix as the S8 and XCover Pro trees) |

## Results (2026-11-19, source kernel `4.9.337-ies-g401189fdfc65`)
| Area | Result |
|---|---|
| pptest | **92 pass, 0 fail, 1 skip**: display 5/5, touch 10/10, power 12/12, keys 8/8 (+1 skip: no top key), usb 11/11, input-modes 16/16, panel 21/21, phonebox 9/9 |
| Power | ~800 mW active (test pattern, brightness 128), ~435 mW dim, 125-150 mW off (panel blanked, CPUs capped) |
| Charge limiter | `batt_slate_mode`, as on the S8 (the phone then runs from the battery) |
| USB | mode B: 3 ACM + RNDIS + adb (`18d1:4ee4`); mode A: 4 HID functions + GUD; switches back and forth cleanly |
| Not yet done | colours by eye (`fb_swap_rb = 1` is expected from LineageOS's `ABGR_8888`), Wi-Fi (BCM4361 B2 firmware in `/vendor/firmware`), auto-rotate, 24 h soak, boot-time trim |
