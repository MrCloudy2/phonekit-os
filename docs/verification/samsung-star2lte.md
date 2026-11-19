# Samsung Galaxy S9+ (SM-G965F, Exynos 9810): port verification

Status: 🟨 2026-11-19. Third PhoneKit device, ported in one session. On the LineageOS 24.0 kernel built from source:
**pptest 92 pass, 0 fail, 1 skip** (no top key). Checked by hand after that: colours, touch and SCREEN by eye,
Wi-Fi, tethering, Bluetooth (keyboard + mouse to an Android phone), DECK, RESCUE. Open: auto-rotate (blocked as on
the S8), a long soak, boot time (paneld starts 3.2 s after the kernel; the S8 does it in 1.4 s).
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
| Colours, touch, SCREEN | by eye (owner): PHONEBOX colours right with `fb_swap_rb = 1`; KDE shows a `USB-1` monitor, the desktop is on the phone and touch works |

## Features checked by hand (2026-11-19)
| Feature | Result |
|---|---|
| Wi-Fi (M10) | BCM4361 B2: firmware from the ROM's `/vendor/firmware` into `/lib/firmware`; WPA2-PSK on 2.4 GHz, DHCP, DNS, 3/3 pings; 130 Mbit/s at −74 dBm; ~12 mW connected and idle |
| Tethering (M10b) | NAT out of `wlan0`; the PC fetched a page through the phone (HTTP 200) while its own default route stayed on Ethernet |
| Bluetooth (M10c) | new Broadcom UART path (`paneld/bt_bcm.c`): up in 0.8 s with the phone's own address; paired with the owner's Android phone; typing and the pointer work; ~40 mW connected |
| DECK (M6h) | 3×4 portrait grid; button 1 reaches the PC as F13 (press + release) |
| RESCUE (M6g) | 119×55 console, 16/256/truecolour, on-screen keys reach the server over the serial port; two key labels cut in portrait ("Dow", "Righ") |
| Rotation (M6e) | manual only: `[SSPBBD]: ssp_down == true`, the hub needs Android's `lhd`, as on the S8 |
| pptest after these changes | 92 pass, 0 fail, 1 skip, no regressions. One earlier full run missed "phone touch arrives on the host at the right position" (no host event in 3 tries); the suite alone passed twice right after, and the next full run passed: intermittent, host side, to watch |
| Boot time | kernel → paneld 3.2 s; no single slow step in dmesg. The bootloader ignores the image cmdline, so `initcall_debug` needs `CONFIG_CMDLINE_EXTEND` (next step) |

## More shared fixes from these checks
| Problem | Fix |
|---|---|
| Wi-Fi firmware is loaded with `request_firmware` (bare names) | `initramfs.spec`: optional `prebuilt/firmware/lib/` → `/lib` |
| `logstrs.bin` missing: 25 s user-helper wait, scan timeout killed `ifconfig` | S9+ kernel fragment: `CONFIG_FW_LOADER_USER_HELPER_FALLBACK` off |
| Programs on the phone couldn't resolve names (no `/etc/resolv.conf` on any phone) | `initramfs.spec`: `/etc/resolv.conf` → `/run/resolv.conf` |
| `phone.sh flash` overwrote the settings store when the partition isn't a multiple of 64 KiB | 4 KiB blocks then; the S8 and XCover keep the same 64 KiB offsets |
| Broadcom UART Bluetooth | `hw.c` `.bt = HW_BT_BCM_UART` + `.bt_uart/.bt_rfkill/.bt_fw` (default SCSC, so the XCover is unchanged); `CONFIG_BT_HCIVHCI` |
| Radios on shared default addresses | optional `prebuilt/firmware/efs-root/` → `/efs` with copies of `bluetooth/bt_addr` and `wifi/.mac.info` |
