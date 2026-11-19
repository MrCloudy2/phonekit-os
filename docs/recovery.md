# Recovery: read this before your first flash

Nearly every problem during a port is a **soft brick**: the phone sticks on the boot logo, bootloops, or
shows a black screen. All of them are recoverable **as long as the bootloader is intact and you have
known-good images**. PhoneKit never touches the bootloader, so keep it that way.

## Before anything else
1. **Find your emergency mode** and try it once while the phone still works:
   - Samsung: download mode (XCover Pro: Vol Up + Vol Down while plugging in USB). Tools: `heimdall` (Linux/macOS) or Odin (Windows).
   - Most others: fastboot (`fastboot devices`), usually Vol Down + Power.
   - MediaTek: BROM/preloader via `mtkclient`, which works even when fastboot doesn't.
2. **Back up every partition you will touch** and keep the copies off the phone, with sha256 sums:
   ```bash
   adb shell su -c 'dd if=/dev/block/by-name/recovery' > recovery-original.img
   ```
   ```bash
   sha256sum recovery-original.img > recovery-original.img.sha256
   ```
   Better: the official image file from the same ROM build (that's what `make import` uses anyway).
3. **Flash recovery, not boot.** Then a broken PhoneKit just means "boot normally into the ROM".

## Safe mode
If PhoneKit starts but a setting makes it unusable (wrong USB mode, a rotation that hides the menu, …), hold
**Volume Down** while it boots: saved settings are ignored for that boot (nothing is erased).

## Symptoms and fixes
| Symptom | Likely cause | Fix |
|---|---|---|
| ROM replaced PhoneKit after one boot | ROM restores recovery (Samsung/LineageOS: `persist.vendor.recovery_update`) | `adb shell su -c 'setprop persist.vendor.recovery_update false'`, flash again |
| Stuck on the boot logo | Kernel hangs early (often after a config trim) | Forced reset (Samsung: Vol Down + Power ~10 s), straight into download/fastboot mode, flash the known-good recovery |
| Bootloops into recovery even after forced reset | Same, the bootloader keeps retrying recovery | Only download/fastboot mode helps: flash known-good |
| Backlight on, picture black | Display not initialised, or wrong pan/blank handling | `make logs` from the ROM; check the device quirks in `socs/` |
| Colours wrong (blue looks brown) | Panel byte order ≠ what fbdev reports | Set `.fb_swap_rb = 1` in `devices/<dev>/hw.c` |
| heimdall: `Failed to find correct interface configuration` in download mode | The PC's adb server (restarted by the PHONEBOX agent) claimed the download-mode interface (`driver=usbfs`), and the phone's download mode is now stuck | `systemctl --user stop phonebox-agent; adb kill-server`, restart download mode on the phone (Vol Down + Power, then the download combo), flash, start the agent again |
| Phone enumerates (`lsusb`) but no `/dev/ttyACM*`, no adb | A VM manager on the PC (virt-manager/SPICE USB auto-redirect) grabbed it: all interfaces show `driver=usbfs` | Un-redirect it in the VM console (Virtual Machine → Redirect USB device), turn auto-redirect off, replug |
| paneld frozen | Usually the watchdog reboots to the ROM within 15 s; else forced reset | `make logs` right away: the next boot overwrites `last_kmsg` |
| "dm-verity"/"AVB" error, refuses to boot | Image size/footer mismatch, or a locked bootloader | Image must be exactly partition-sized with a footer (`AVB_FOOTER := 1`), bootloader must be unlocked |

## Getting logs from a boot that failed
From the ROM, right after the failed boot (before booting anything else):
```bash
make logs
```
If there is no ROM to boot into, flash the known-good recovery (TWRP/LineageOS recovery with adb) and read
`/proc/last_kmsg` (or `/sys/fs/pstore/console-ramoops*`) from there.

## Things that can cause a real (hard) brick, so don't do them
- Flashing anything to `bootloader`, `sboot`, `abl`, `xbl`, `preloader`, `lk`, `tz`, or modem partitions.
- Relocking the bootloader with a non-stock boot/recovery image on the phone.
- Flashing an image built for a different model, even a "close" variant.
- Running a phone without battery protection: always-plugged lithium cells swell. Keep the charge limiter on.
