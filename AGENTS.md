# Instructions for AI coding agents

You are working on phonepanel: a minimal Linux image (vendor kernel + one static C daemon, `paneld`, as PID 1)
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
- Record what you verified in `docs/verification/` and keep `ROADMAP.md` statuses honest.

## Layout
`paneld/` daemon (C, static) · `devices/<vendor>-<codename>/` per-phone data · `socs/<soc>/` shared quirks ·
`scripts/` build/flash/test tools · `protocol/` PPJ/1 spec · `host/` host clients · `examples/` · `docs/`.
