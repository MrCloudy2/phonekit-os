# Samsung Galaxy S8 (SM-G950F, Exynos 8895): port verification

Status: 🟨 2026-11-19. Second PhoneKit device, ported in one session. On a kernel built from source: boots, touch,
display, SCREEN (USB monitor + touch), MOUSE/KEYBOARD/COMBO, PHONEBOX with the stock agent, PPJ/1, Wi-Fi, charge
limiter. Open: autorotate (blocked by Android's GPS-chip daemon), screen-off power floor (no system suspend),
battery capacity measurement, a long soak. Full facts with sources: `devices/samsung-dreamlte/device.yaml`;
chip-wide findings: `socs/exynos8895/quirks.md`. Latest `scripts/pptest.py` report: `samsung-dreamlte-report.md`.

## How it was done
| Step | What happened |
|---|---|
| Stage 0 | SM-G950F (Exynos) confirmed over adb; bootloader already unlocked (`ro.boot.flash.locked=0`, orange), stock Android 9 `G950FXXUCDVG4`. The US Snapdragon S8 (SM-G950U/U1/W) can't unlock |
| Backups | Official TWRP 3.7.0 for `dreamlte` (sha256 checked against twrp.me); stock `G950FXXUCDZE7` recovery + boot from Samsung's update server (the only build it serves), sha256s kept off the phone |
| Partition | `RECOVERY` (upper case), 48 234 496 B: PIT `heimdall print-pit` (11 776 × 4 KiB UFS blocks), confirmed with `blockdev` in TWRP |
| Boot image | Samsung header v0 + DT table (`DTBH`): AOSP `unpack_bootimg` crashes on it. New `scripts/samsung-bootimg.py` unpacks/packs it; the TWRP image round-trips byte for byte. No AVB |
| First boot | TWRP kernel 4.4.111 + PhoneKit ramdisk: test pattern + touch drawing on the first try; colours swapped → `FB_SWAP_RB := 1` |
| ROM | /e/OS 4.4.1 (Android 14, LineageOS 21 based, unofficial, `userdebug` → adb root) installed from TWRP, so `make deploy` / `make logs` work without buttons |
| Kernel | Built from source: `8890q/android_kernel_samsung_universal8895` `lineage-21` (4.4.302) with HID + mass storage, host clang 23 + GNU ld. Three patches (two real backport bugs clang 23 caught). DT table from the /e/OS recovery. paneld starts 1.4 s after the kernel (TWRP kernel: 2.2 s) |

## What needed changes (all generic, no `#ifdef <phone>`)
| Problem on the S8 | Fix |
|---|---|
| USB gadget never bound: `failed to start pp: -22` | Samsung composite gadget layer: functions join the config only via `/sys/class/android_usb/android0/functions` + `enable`. paneld detects it at runtime (`gadget.c`) |
| Samsung names every FunctionFS function `adb` | paneld maps `ffs.*` to `adb` on that layer |
| No `battery/charging_enabled` | `CHARGE_CTL := batt_slate_mode`, `CHARGE_CTL_INVERT := 1` in `device.mk` (new build variables) |
| Backlight 0..36600, paneld wrote XCover values (0..306) → 0.3 % brightness | paneld scales its brightness to `max_brightness` |
| No XCover/top key; Bixby key unused | `bixby` key (0x2bf) with the XCover key's defaults; on-screen hints from `MODE_KEY_LABEL` / `PAGE_KEY_LABEL` |
| No devtmpfs in the TWRP kernel → no `/dev/urandom` → WPA handshake fails | paneld creates the `/sys/class/mem` nodes when devtmpfs is missing |
| CHUB power-on path hard-coded, log spam | paneld looks for a `*.contexthub` device and only powers it on if there is one |
| `make update` refused non-AVB images | `mkimage.sh` pads AVB-less images to the partition size |
| Flash/backup scripts assumed `/dev/block/by-name` (missing in TWRP) | `phone.sh` also checks `/dev/block/platform/*/by-name` |
| `import-prebuilt.sh` deleted `prebuilt/firmware/` on re-import | it now replaces only its own files |
| Test suites hard-coded XCover values (1080×2340, frequencies, keys, page numbers, tap points) | suites read the phone's features (`ppctl info`: `fb`, `keys`; `ppctl usb`: `hid`), SKIP what's absent; rule added to AGENTS.md / CONTRIBUTING.md |
| Tests picked the host's media-key device as the touchscreen/mouse (it has an ABS and a REL bit) | pick by capability: multitouch X for the touchscreen, REL X+Y + BTN_LEFT for the mouse |
| `pptest.py` counted every failure twice (the suite's "FAILED:" summary line) | only `PASS `/`FAIL `/`SKIP ` result lines count |
| With devtmpfs, every USB mode switch deleted and recreated `/dev/hidgN`; paneld kept writing to the deleted nodes, so MOUSE/KEYBOARD died after the first switch | paneld reopens HID nodes after every bind (`gadget_bind_gen()`) |
| The PHONEBOX client switched to its page when the first dashboard arrived, even in test mode | it respects `phonebox mute` |
| The phonebox agent kept the phone awake / on its page during tests | `ppctl phonebox mute 1` (runtime test mode); `pptest.py` mutes only the phone under test |

## Results (2026-11-19)
| Area | Result |
|---|---|
| pptest | **91 pass, 1 fail, 1 skip** on the source kernel: display 5/5, touch 10/10, power 11/12, keys 8/8 (+1 skip: no top key), usb 11/11, input-modes 16/16, panel 21/21, phonebox 9/9. (TWRP kernel: 72/1/3, HID skipped) |
| Power | active 954 mW (brightness 128), dim 576 mW, screen off **251 mW** (panel really off, CPUs capped, modem never started): the Exynos 8895 floor without system suspend; `off < 1/5 of active` fails |
| Charge limiter | `batt_slate_mode` stops charging (the phone then runs from the battery: 60 → 40 → 60 % cycle) |
| Wi-Fi | BCM4361: `bcmdhd_sta.bin_b0` + `nvram.txt_r02j_b0` from the ROM; WPA2-PSK, DHCP, DNS, 3/3 pings at 15 ms; ~145 mW extra at −85 dBm (one measurement; re-measure) |
| PHONEBOX | stock agent 0.9.14 over USB: pairs, live stream, 7 pages, taps → POSTs |
| SCREEN (GUD) | ✅ on the source kernel: state committed, frames on the panel, touch lands where it should (owner confirmed with KDE). On the TWRP kernel frames never reached the panel (no commit seen, function resets, once a stalled control pipe) |
| HID modes | ✅ on the source kernel: MOUSE, KEYBOARD, COMBO 16/16; SCREEN touch 11/11. The TWRP and /e/OS kernels have no `CONFIG_USB_CONFIGFS_F_HID` |
| Rotation | manual: all 4 orientations correct. Auto: blocked; the sensor hub runs on the BCM4775 GPS chip behind Android's `lhd` daemon (ROADMAP M6e) |
| Battery | gauge says 2967 / 3000 mAh, health Good, but its learned values look untouched; coulomb-counting measurement with `scripts/battery-capacity.py` running |
