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

## Other phones (when `paneld/`, `scripts/` or `initramfs/` changed)
<!-- Which phones' paneld-config.txt / footprint.txt changed in this diff, and why. Then out/check-all.md from
     `make check-all` with the phones you have (nobody is expected to own every phone). -->

- [ ] Per-phone behaviour is in `devices/<dev>/hw.c` (new fields default to the old behaviour), no `#ifdef <phone>`
- [ ] `devices/*/paneld-config.txt` and `devices/*/footprint.txt`: only the phones this PR means to change differ, or the PR says why the others do
- [ ] `make check-all`: no regressions on the phones I have
