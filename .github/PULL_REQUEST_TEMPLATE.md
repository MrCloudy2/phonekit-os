## What
<!-- New device / SoC notes / example / fix -->

## Device (for ports)
- Model + model number:
- SoC (`socs/<soc>`):
- Kernel: prebuilt from <image> / source <repo@branch>

| Stage | Works? | How tested |
|---|---|---|
| Boots to test pattern | | |
| Touch | | |
| USB control channel (`ppctl.py info`) | | |
| Colours correct | | |
| Charge limiter | | |
| SCREEN / MOUSE / KEYBOARD modes | | |
| PANEL mode | | |
| Rotation | | |

- [ ] No vendor binaries (kernel, DTB, firmware, images) in this PR
- [ ] Quirks recorded in `device.yaml` (and `socs/<soc>/quirks.md` if SoC-wide)

## Other phones (required when `paneld/` or `scripts/` changed)
<!-- paste out/check-all.md from `make check-all` -->

- [ ] Per-phone behaviour is in `devices/<dev>/hw.c` (new fields default to the old behaviour), no `#ifdef <phone>`
- [ ] `devices/*/paneld-config.txt`: only the phones this PR means to change differ (`make device=<dev> config-check`)
- [ ] `make check-all`: no regressions on any phone I have
