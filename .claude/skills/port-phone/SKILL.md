---
name: port-phone
description: Port PhoneKit to a new phone or install it on a supported one. Use when the user names a phone model they want to run PhoneKit on, asks whether their phone can be done, or is in the middle of a port (black screen, wrong colours, no USB, no touch, kernel build failing). Walks the porting guide stage by stage with backups and the repo's tools, and asks before every flash.
---

# Port PhoneKit to a phone

Follow `docs/porting-guide.md` stage by stage. This skill adds the order of work and the rules; the guide has the
details. Read `AGENTS.md` and `docs/recovery.md` before doing anything that touches the phone.

## Rules (non-negotiable)
- **Feasibility first, no flashing**: unlockable bootloader, a recovery/ROM with a working display (TWRP, LineageOS,
  postmarketOS) for the *exact* model number, its image file, a working emergency mode (download/fastboot/BROM),
  arm64. If any is missing, say so plainly and stop.
- **Backups before every first**: a known-good image of every partition you will write, with sha256, off the phone.
  Have the user try the emergency mode once.
- **Ask before** unlocking, every flash, every partition write, every kernel config trim. Say how to undo it.
- **Only** the partition in `device.mk` (`TARGET_PARTITION_NAME`, normally `recovery`). Never bootloader, modem,
  TrustZone, preloader, `efs`, `persist`. Never relock a bootloader.
- **Never type the user's passwords** (Wi-Fi, accounts): give them the command to run.
- Vendor binaries (kernels, firmware, images) go to `devices/<dev>/prebuilt/` (gitignored), never into git.

## Order of work
1. **Is there prior work?** `devices/` for the model, `socs/<soc>/quirks.md` for the chip. Reuse it.
2. **Facts from the phone**: in its working recovery or ROM with root, run `scripts/probe.sh`
   (`adb push scripts/probe.sh /tmp/ && adb shell sh /tmp/probe.sh`). It drafts `device.yaml`.
3. **Device folder**: `cp -r devices/_template devices/<vendor>-<codename>`; fill `device.mk` (partition name and size,
   flash command, `FB_SWAP_RB`) and `device.yaml` from the probe. Personal paths go in `local.mk` (gitignored).
4. **Import + build**: `make import device=<dev> IMG_SRC=<known-good recovery.img>`, `make device=<dev>`. Compare the
   header with `unpack_bootimg` on both images.
5. **First boot** (ask first): flash recovery, boot it. Gate: test pattern with a red border and touch drawing.
   Black screen with backlight → `make logs` from the ROM straight away; wrong colours → `FB_SWAP_RB := 1`;
   ROM replaced recovery → `persist.vendor.recovery_update=false`.
6. **Control channel**: `scripts/ppctl.py --wait 90 info`. From here, iterate with `make update` (self-update, ~15 s)
   and `make deploy` (from the ROM) — no buttons.
7. **Quirks**: `scripts/pptest.py` runs every suite and writes `docs/verification/<dev>-report.md`. Fix failures one
   at a time; device differences become `device.mk` variables / `-D` defines, never `#ifdef <phone>`. SoC-wide
   findings go in `socs/<soc>/quirks.md`.
8. **Optional**: kernel from source (`KERNEL_SRC`, `KERNEL_FRAGMENTS`, patches as `git format-patch`), trim one thing
   at a time with `initcall_debug`; Wi-Fi firmware into `prebuilt/firmware/` (see `docs/verification/M10.md`).
9. **Record**: `scripts/devicedb.py check` and `scripts/devicedb.py table`; mark only verified values
   `verified: true`; help the user open a PR (partial ports welcome) using `.github/PULL_REQUEST_TEMPLATE.md`.

## When stuck
- Logs: `make logs` (previous boot's `last_kmsg`), `/sys/fs/pstore/console-ramoops-0`, `ppctl dmesg`.
- paneld records why it rebooted (`previous reboot by paneld:` at boot).
- Safe mode: Vol Down held at boot ignores saved settings.
- Suspect the host too (the first freeze of the reference port was a joystick daemon writing into the serial port).
