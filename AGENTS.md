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
- A port must not break the phones that already work. Shared code (`paneld/`, `scripts/`) changes behaviour only
  through a `device.mk` variable whose default is the old behaviour, or through runtime detection that falls back
  to the old path (example: the Samsung composite gadget path runs only where `/sys/class/android_usb/android0`
  exists). CI builds and starts paneld with every `devices/*/device.mk`. On hardware, `scripts/pptest.py` ends with
  `scripts/ppregress.py`: a check that passed in `devices/<dev>/pptest-baseline.txt` and now fails is a regression
  and blocks the change. Update a baseline (`ppregress.py <dev> --update`) only after a verified run, and say in
  the PR which phones you ran pptest on.
- Document as you port, not at the end: every fact you learn about the phone (with its source and date) goes into
  `device.yaml`, every chip-wide finding into `socs/<soc>/quirks.md`, every new symptom and fix into
  `docs/porting-guide.md` or `docs/recovery.md`. That includes dead ends and tool limitations you hit. The next
  port starts from what you wrote down.

## Layout
`paneld/` daemon (C, static) · `devices/<vendor>-<codename>/` per-phone data · `socs/<soc>/` shared quirks ·
`scripts/` build/flash/test tools · `protocol/` PPJ/1 spec · `host/` host clients · `examples/` · `docs/`.
