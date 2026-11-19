# Contributing

The most valuable contribution is **another phone**. Partial ports count: "display and touch work, USB
doesn't" in a device folder saves the next person days.

## Adding a phone
1. `cp -r devices/_template devices/<vendor>-<codename>` (e.g. `samsung-a50`, `xiaomi-lavender`).
2. Follow [docs/porting-guide.md](docs/porting-guide.md) (or paste [PROMPT.md](PROMPT.md) into an AI assistant).
3. Put SoC-wide findings in `socs/<soc>/quirks.md` (create it if it's a new chip) and link your device there.
4. In your PR, include:
   - `devices/<vendor>-<codename>/` with `device.mk`, `device.yaml`, and any `kernel/` fragment or `kernel-patches/`
   - which stages work (template below), with how you tested them
   - a photo or `ppctl.py screenshot` of it running (optional, appreciated)
5. **Never commit vendor binaries**: no kernels, DTBs, firmware, or full images. `devices/*/prebuilt/` and
   `devices/*/local.mk` are gitignored for that reason. Kernel patches are fine (GPL-2.0).

## Device-specific code
Prefer data over code. If your phone needs a different sysfs path or behaviour, set it in your
`devices/<dev>/hw.c`, or add a field to `struct phone_hw` (`paneld/hw.h`) whose default keeps the old behaviour.
Don't add `#ifdef SAMSUNG_A50`-style blocks, and don't guess the phone type at runtime.

## Tests
The suites in `scripts/test-*.py` (run all of them with `scripts/pptest.py`) must work on every phone. Don't
hard-code one phone's values (resolution, CPU frequencies, key names, charge behaviour): read them from the phone
(`ppctl info` reports `fb WxH` and the hardware `keys`, `ppctl usb` reports `hid yes|no`) and print
`SKIP  <check>  <why>` when the phone lacks a feature. A check that only passes on your phone is a bug in the test.
A suite must not depend on what ran before it: `pptest.py` puts the phone in USB mode B (PHONEBOX, the phone draws
its own screen) before every suite, and a suite that needs another mode sets it itself. Keep re-enumerations few:
every one is a chance to hit a controller bug (the Exynos 9611 DWC3 can hang after many soft replugs), so stress
loops are opt-in flags (`test-usb.py --replugs 50`), not the default.

## Don't break other phones
Three layers, cheapest first:
1. **Design.** What paneld does differently per phone lives in `devices/<dev>/hw.c` (fields in `paneld/hw.h`; see
   the porting guide, "The hardware layer"). A new field's default in `HW_DEFAULTS` is the old behaviour, so other
   phones don't move. No `#ifdef <phone>`, and no guessing the phone type at runtime.
2. **CI, no hardware.** Every device builds, starts under qemu, and CI compares two files per phone:
   `devices/<dev>/paneld-config.txt` (what paneld does on it) and `devices/<dev>/footprint.txt` (what goes onto it and
   how: build settings, kernel config, image contents, flash layout, and the USB gadget paneld builds at boot and per
   mode, from `PANELD_PRINT_USB_PLAN`). If your diff changes another phone's file, your change reaches that phone. Say
   why in the PR, or make it opt-in through `hw.c`. `make device=<dev> config-update footprint-update` refreshes them.
3. **Hardware, for the phones you have.** `make check-all` runs `scripts/pptest.py` on every connected phone against
   its `pptest-baseline.txt` and fails on any regression. Paste `out/check-all.md` into the PR. Nobody owns every
   phone: you don't owe runs on phones you don't have. Each phone's owner re-verifies on their own schedule, and
   `docs/devices.md` shows the commit each phone last passed at ("Verified at"). Build that commit if you want an image
   that was tested on your phone.

After a verified run with new passes, refresh the baseline: `scripts/ppregress.py <device> --update`.

## Other contributions
Examples (`examples/<name>/`), host clients in other languages, protocol ideas (open an issue first), and
fixes to paneld. Run `make test` (qemu) before sending C changes; on hardware, the `scripts/test-*.py` suites.

## Style
- C: plain C11, no dependencies beyond libc, static build. Keep paneld small.
- Docs: short sentences, say what's verified and what isn't.
