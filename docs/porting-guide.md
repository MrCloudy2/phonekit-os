# Porting guide: bringing PhoneKit to a new phone

Plain-text version of what [PROMPT.md](../PROMPT.md) asks an AI assistant to do. Work through the stages in
order, and don't skip a gate. Each stage ends with something you can see on the phone.

> You are flashing modified images. Read [recovery.md](recovery.md) first and keep known-good images.
> There is no guarantee your phone can be done. Stage 0 tells you early.

## Stage 0: Can this phone be done? (no flashing yet)
Go only if **all** of these are yes:
- [ ] **The bootloader unlocks** officially (OEM unlock / `fastboot flashing unlock`) or via a well-known tool.
      Carrier-locked US phones and most recent Huawei phones: stop here.
- [ ] **A recovery or ROM with a working display exists**: TWRP, LineageOS recovery, OrangeFox, or
      postmarketOS. That proves a kernel can drive the panel without Android. Check the LineageOS wiki, the TWRP
      device list, XDA, and the postmarketOS wiki for your **exact** model number.
- [ ] **You have its image file** (`recovery.img` or `boot.img`), or can dump it from the phone.
- [ ] **An emergency flash mode works** (download/fastboot/BROM). Try it now.
- [ ] arm64 (aarch64). Nearly every phone since 2016 is.

Nice to have: kernel source for that ROM (for trimming and adding gadget functions), a removable battery,
and a userdebug/rooted ROM for `make deploy`.

## Stage 1: Create the device folder
Tip: `scripts/probe.sh` reads most hardware facts from the running phone (in TWRP/LineageOS recovery with adb, or
later under PhoneKit) and prints a `device.yaml` draft: `adb push scripts/probe.sh /tmp/ && adb shell sh /tmp/probe.sh`.

```bash
cp -r devices/_template devices/<vendor>-<codename>
```
Fill in `device.mk` (partition, flash tool, size) and as much of `device.yaml` as you know. If your SoC has a
folder in [`socs/`](../socs/), read its `quirks.md`: most of what you'll hit is there already. If not, create
one (`socs/<soc>/quirks.md`), because the next person with that chip will thank you.

Find the partition size:
```bash
adb shell su -c 'blockdev --getsize64 /dev/block/by-name/recovery'
```

## Stage 2: Import and build
```bash
make import device=<vendor>-<codename> IMG_SRC=/path/to/recovery.img
```
```bash
make device=<vendor>-<codename>
```
`make import` keeps the kernel, DTB and DTBO byte for byte. Compare the header: `unpack_bootimg` on both the
original and `out/<device>/phonepanel.img` should show the same header version, page size and offsets.

## Stage 3: First boot: the gate is "test pattern visible"
Flash to **recovery** (`make flash`, or `make deploy` from a rooted ROM) and boot recovery.
Expected: a test pattern with a red border on all four edges and a clock, and touch draws under your finger.
- **Black screen, backlight on**: the display path isn't initialised. Get logs (recovery.md), look for the
  fbdev/DRM node. Today paneld drives fbdev (`/dev/fb0`); a DRM/KMS backend is still to come (M3 notes).
- **Wrong colours**: `FB_SWAP_RB := 1`.
- **Back in the ROM's recovery after one boot**: the ROM restores recovery; see recovery.md.
- **Logo forever**: that kernel/ramdisk combo doesn't boot. Check the header/offsets and the AVB footer.

## Stage 4: USB control channel
After boot, the PC should see `1d6b:0104 phonepanel` with three serial ports.
```bash
scripts/ppctl.py --wait 90 info
```
From here you can work without buttons: screenshots (`ppctl.py screenshot out.png`), touch injection, a shell, and logs.
If nothing enumerates, check the UDC name: `ls /sys/class/udc` in the shell (or from TWRP).

## Stage 5: Find the quirks
Run everything with `scripts/pptest.py` (writes a report for your PR), or the suites one at a time, and fix what fails. Record every finding in `device.yaml` → `quirks:`
and, if it's SoC-wide, in `socs/<soc>/quirks.md`:
```bash
scripts/test-display.py
```
```bash
scripts/test-touch.py
```
```bash
scripts/test-usb.py
```
```bash
scripts/test-power.py
```
```bash
scripts/test-keys.py
```
```bash
scripts/test-input-modes.py
```
Typical device-specific things:
| Area | Where it shows up | XCover Pro example |
|---|---|---|
| Colour order | test-display | DECON hard-codes ABGR → `FB_SWAP_RB` |
| Wake from blank | screen black after long off | DECON needs `FBIOPUT_VSCREENINFO` after unblank |
| Touch node / max points | test-touch | `sec_touchscreen`, 10 slots |
| Buttons | test-keys | Vol Up/Down, Power, XCover key, top key |
| Charger control | test-power | `/sys/class/power_supply/battery/charging_enabled` |
| Rotation sensor | Settings → Rotation AUTO | CHUB sensor hub, `device_orientation` |
| Network gadget | PANEL mode ping | RNDIS works, NCM doesn't transmit |

Some of these paths are still hard-coded in paneld (`paneld/sensors.c`, `paneld/power.c`), because there has only been one
device so far. When yours differs, move the value into `device.mk` as a `-D` define (see how `FB_SWAP_RB`
reaches `fb.c` through the Makefile) instead of adding `#ifdef <your phone>`. That way the next port stays
data, not code.

## Stage 6 (optional): Kernel from source
Needed to trim boot time or to enable gadget functions the ROM's kernel lacks (`CONFIG_USB_CONFIGFS_F_HID`
for MOUSE/KEYBOARD/SCREEN touch, `CONFIG_USB_CONFIGFS_MASS_STORAGE`).
- Point `KERNEL_SRC`/`KERNEL_DEFCONFIG` in `device.mk` at the ROM's kernel tree, and put build fixes in
  `kernel-patches/` as `git format-patch` files.
- Add your changes as a config fragment in `kernel/`. **Trim one thing at a time**, measure with
  `initcall_debug`, and keep the known-good image ready. Secure-world-owned hardware (NFC/eSE, fingerprint)
  can hang the kernel if disabled.

## Stage 7: Record and contribute
- `scripts/devicedb.py check` validates your record; `scripts/devicedb.py table` refreshes `docs/devices.md` (CI checks both).
- Fill in `device.yaml` with every verified fact (`verified: true` only when you tested it).
- Add `docs/verification/<device>.md` with what you tested, or extend the table in the README.
- Open a pull request; see [CONTRIBUTING.md](../CONTRIBUTING.md). Partial ports (say, display and touch only)
  are welcome. Mark how far you got.
