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

## Other contributions
Examples (`examples/<name>/`), host clients in other languages, protocol ideas (open an issue first), and
fixes to paneld. Run `make test` (qemu) before sending C changes; on hardware, the `scripts/test-*.py` suites.

## Style
- C: plain C11, no dependencies beyond libc, static build. Keep paneld small.
- Docs: short sentences, say what's verified and what isn't.
