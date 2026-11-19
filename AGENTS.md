# Instructions for AI coding agents

You are working on PhoneKit: a minimal Linux image (vendor kernel + one static C daemon, `paneld`, as PID 1)
that turns an old phone into a touch panel. Users flash it to real phones. Mistakes can soft-brick a phone.

## Safety rules (non-negotiable)
- Before flashing any partition for the first time, make sure a known-good image of it exists off the phone,
  with a sha256. Tell the user how to reach the emergency mode (download/fastboot) first.
- Ask the user before every flash, bootloader unlock, or partition write, and say how to undo it.
- Only ever write the partition named in `device.mk` (`TARGET_PARTITION_NAME`, normally `recovery`). Never
  bootloader, modem, TrustZone, preloader, `persist`, or `efs`-type partitions. Never relock a bootloader.
- Keep the charge limiter enabled by default. Don't weaken battery/thermal protection.
- Never commit vendor binaries (`devices/*/prebuilt/`) or personal paths (`devices/*/local.mk`).

## How to work
- Read `docs/porting-guide.md`, `docs/recovery.md`, `socs/<soc>/quirks.md` and the device's `device.yaml` first.
- Use the repo tools: `make import|deploy|flash|logs|test`, `scripts/ppctl.py`, `scripts/test-*.py`.
- After a failed boot, read `make logs` immediately (the next boot overwrites `last_kmsg`).
- Verify on hardware with screenshots and test suites; ask the user about things only eyes can check.
- Device differences go into `device.mk`/`device.yaml` as data, not `#ifdef <device>` in paneld.
- Test suites (`scripts/test-*.py`) are general, never written for one phone: no hard-coded resolution, CPU
  frequencies, key names, sysfs paths or charge behaviour. Ask the phone first (`ppctl info`: `fb WxH`, `keys ...`;
  `ppctl usb`: `hid yes|no`), test what it has, and print `SKIP  <check>  <why>` for what it lacks. If a suite needs
  a fact paneld doesn't report yet, make paneld report it (detected at runtime, or from `device.mk`).
- Record what you verified in `docs/verification/` and keep `ROADMAP.md` statuses honest.
- A port must not break the phones that already work. Per-phone behaviour lives in `devices/<dev>/hw.c` (the
  table in `paneld/hw.h`). A new field gets a default in `HW_DEFAULTS` that keeps the old behaviour, so other phones
  don't change. No `#ifdef <phone>` and no runtime guessing of the phone type.
- Nobody can test every phone on every change, so CI shows what a change reaches instead. For every device it builds
  and starts paneld and compares two files: `devices/<dev>/paneld-config.txt` (what paneld does: `hw.c` + defaults)
  and `devices/<dev>/footprint.txt` (what goes onto the phone and how: build settings, kernel config, image contents,
  flash layout, the USB gadget per mode; `scripts/footprint.py`). If another phone's file changes in your diff, your
  change reaches that phone: say why in the PR, or make it opt-in through `hw.c`. Refresh them with
  `make device=<dev> config-update footprint-update`.
- On hardware, run `make check-all` with the phones you have; it compares each with its `pptest-baseline.txt`, and a
  check that passed there and now fails blocks the change. You don't owe a run on phones you don't have: each phone's
  owner re-verifies later, and `docs/devices.md` shows the commit every phone last passed at ("Verified at", from the
  baseline). Update a baseline (`ppregress.py <dev> --update`) only after a verified, committed run, and paste
  `out/check-all.md` into the PR.
- Document as you port, not at the end: every fact you learn about the phone (with its source and date) goes into
  `device.yaml`, every chip-wide finding into `socs/<soc>/quirks.md`, every new symptom and fix into
  `docs/porting-guide.md` or `docs/recovery.md`. That includes dead ends and tool limitations you hit. The next
  port starts from what you wrote down.

## Layout
`paneld/` daemon (C, static) · `devices/<vendor>-<codename>/` per-phone data · `socs/<soc>/` shared quirks ·
`scripts/` build/flash/test tools · `protocol/` PPJ/1 spec · `host/` host clients · `examples/` · `docs/`.
