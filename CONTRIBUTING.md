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
Prefer data over code. If your phone needs a different sysfs path or behaviour, add a `device.mk`
variable that becomes a `-D` define (see `FB_SWAP_RB` in the Makefile → `paneld/fb.c`), or detect it at
runtime. Don't add `#ifdef SAMSUNG_A50`-style blocks.

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
Every device folder has a `pptest-baseline.txt`: what that phone passed last time. `scripts/pptest.py` compares
each run against it (`scripts/ppregress.py`) and lists regressions. Rules for changes to shared code (`paneld/`,
`scripts/`):
- New behaviour for your phone comes from a `device.mk` variable that defaults to the old behaviour, or from runtime
  detection that keeps the old path where the feature isn't there.
- CI builds and starts paneld with every device's `device.mk`; it has to stay green for all of them.
- Run pptest on every phone you have, and list them in the PR. Maintainers run it on theirs before merging; a
  regression on any phone blocks the merge until it's fixed.
- After a verified run with new passes, refresh the baseline: `scripts/ppregress.py <device> --update`.

## Other contributions
Examples (`examples/<name>/`), host clients in other languages, protocol ideas (open an issue first), and
fixes to paneld. Run `make test` (qemu) before sending C changes; on hardware, the `scripts/test-*.py` suites.

## Style
- C: plain C11, no dependencies beyond libc, static build. Keep paneld small.
- Docs: short sentences, say what's verified and what isn't.
