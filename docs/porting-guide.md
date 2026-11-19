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
Fill in the three files, each with its own job:
- `device.mk`: how to build and flash (partition, flash tool, size).
- `hw.c`: how paneld drives the hardware (colour order, charging attribute, mode/page keys, USB gadget type, sensor
  hub). Leave the template values and fix them as you find out; see "The hardware layer" under Stage 5.
- `device.yaml`: as many facts as you know, with their sources. If your SoC has a
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
- **Wrong colours**: `.fb_swap_rb = 1` in `devices/<dev>/hw.c`.
- **Back in the ROM's recovery after one boot**: the ROM restores recovery; see recovery.md.
- **Logo forever**: that kernel/ramdisk combo doesn't boot. Check the header/offsets and the AVB footer.

## Stage 4: USB control channel
After boot, the PC should see `1d6b:0104 phonepanel` with three serial ports.
```bash
scripts/ppctl.py --wait 90 info
```
From here you can work without buttons: screenshots (`ppctl.py screenshot out.png`), touch injection, a shell, and logs.
If nothing enumerates, check the UDC name: `ls /sys/class/udc` in the shell (or from TWRP).
If the log says `failed to start pp: -22` or `needs at least one function`, the kernel has Samsung's composite
gadget layer (`/sys/class/android_usb/android0/`). paneld detects it and binds through `android0/functions` +
`enable`; see [socs/exynos8895/quirks.md](../socs/exynos8895/quirks.md) for how it works.
If the gadget comes up at boot but the first USB mode switch leaves only the serial ports (no `/dev/hidg*`, no
network, no adb) and the kernel log says `ADB Only mode for recovery or VTS test`, the kernel wants the config named
`Conf 1`: set `.usb_config_name = "Conf 1"` in `hw.c` (Exynos 9810, see
[socs/exynos9810/quirks.md](../socs/exynos9810/quirks.md)).

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
The suites read the phone's features first (screen size and hardware keys from `ppctl info`, HID support from
`ppctl usb`) and report `SKIP` for what your phone doesn't have. A `FAIL` means something is wrong on your phone,
or a suite still assumes another phone: fix the suite to ask the phone, don't special-case yours (see
CONTRIBUTING.md → Tests).

Typical device-specific things:
| Area | Where it shows up | XCover Pro example |
|---|---|---|
| Colour order | test-display | DECON hard-codes ABGR → `hw.c` `fb_swap_rb` |
| Wake from blank | screen black after long off | DECON needs `FBIOPUT_VSCREENINFO` after unblank |
| Touch node / max points | test-touch | `sec_touchscreen`, 10 slots |
| Buttons | test-keys | Vol Up/Down, Power, XCover key, top key |
| Charger control | test-power | `hw.c` `charge_ctl`: `charging_enabled` (S8: `batt_slate_mode`, inverted) |
| Rotation sensor | Settings → Rotation AUTO | CHUB sensor hub, `device_orientation` |
| Network gadget | PANEL mode ping | RNDIS works, NCM doesn't transmit |
| `/dev` without devtmpfs | Wi-Fi never joins (`TEMP-DISABLED`) | Galaxy S8's TWRP kernel: no devtmpfs, so no `/dev/urandom` for wpa_supplicant; paneld now creates the memory nodes itself |
| USB gadget layer | `failed to start pp: -22` | Galaxy S8: Samsung composite (`android0/functions` + `enable`): `hw.c` `.usb = HW_USB_SAMSUNG_COMPOSITE` |
| Sensor hub | rotation never changes | Galaxy S8: hub runs on the GPS chip behind Android's `lhd` daemon, so manual rotation only |
| USB config name | first mode switch drops HID/network/adb, `ADB Only mode` in dmesg | Galaxy S9+: Exynos 9810 composite treats any config name but `Conf 1` as a GSI image: `hw.c` `.usb_config_name = "Conf 1"` |
| Wi-Fi firmware via `request_firmware` | `Direct firmware load for bcmdhd_sta.bin_b2 failed with error -2` | Galaxy S9+: files in `prebuilt/firmware/lib/firmware/` (-> `/lib/firmware`), not `wifi/` |
| A missing optional firmware file stalls a driver | 25 s pause at `Falling back to user helper` | Galaxy S9+: `# CONFIG_FW_LOADER_USER_HELPER_FALLBACK is not set` in the kernel fragment |
| Broadcom Bluetooth on a UART | no `scsc_h4`, an rfkill `bcm43xx Bluetooth`, a `*.hcd` in the ROM's firmware | Galaxy S9+: `hw.c` `.bt = HW_BT_BCM_UART`, `.bt_uart`, `.bt_rfkill`, `.bt_fw`; `CONFIG_BT_HCIVHCI` |
| Shared default radio addresses | Bluetooth `43:61:B2:00:1F:AC`, Wi-Fi `00:90:4c:..` | copy the ROM's `/efs/bluetooth/bt_addr` (and `/efs/wifi/.mac.info`) into `prebuilt/firmware/efs-root/` (-> `/efs` in the image) |
| Newer kernel than your clang expects | `-Werror` stops the build on diagnostics the tree's own clang didn't have | Galaxy S9+: `KERNEL_KCFLAGS` in `device.mk` (several flags; `KERNEL_MAKE_FLAGS` splits on spaces) |

### The hardware layer: `devices/<dev>/hw.c`
Everything paneld does differently per phone is one table, `struct phone_hw` (fields and meanings in
`paneld/hw.h`). Copy `devices/_template/hw.c`, fill it in, and list every field explicitly. Each part of the
device folder has its own job:
- `device.mk`: how to build and flash the phone (kernel, partition, flash command).
- `hw.c`: how paneld drives the hardware (colour order, charging attribute, mode/page keys and labels, USB gadget
  type, sensor hub power).
- `device.yaml`: facts and their sources.

When your phone needs something paneld can't express yet:
1. Add a field to `struct phone_hw`, with a value in `HW_DEFAULTS` that keeps the old behaviour. Every other phone
   is then unchanged without anyone touching its file.
2. Set the field in your `hw.c`, and use it in shared code through `phone.<field>`. Never write `#ifdef <your phone>`,
   and never guess the phone type at runtime ("this sysfs file exists, so it must be ...").
3. Run `make device=<dev> config-update footprint-update` for each phone and commit `devices/*/paneld-config.txt` and
   `devices/*/footprint.txt`. They are what paneld does on the phone and what goes onto it, and CI fails when they
   change without the commit. A change for one phone should only change that phone's files.

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
- `make device=<dev> config-update footprint-update` writes `devices/<dev>/paneld-config.txt` (what paneld does on
  your phone) and `devices/<dev>/footprint.txt` (what goes onto it: build settings, kernel config, image, flash
  layout, the USB gadget per mode). CI compares both on every change, so later changes can't reach your phone
  unnoticed. Check they read right, then commit them.
- After a verified `scripts/pptest.py --device <dev>` run, create your phone's baseline with
  `scripts/ppregress.py <dev> docs/verification/<dev>-report.md --update`. From then on, nobody's change can quietly
  break what your phone passes.
- `make check-all` with every phone you have plugged in. Shared code you touched must not regress the others.
  Paste `out/check-all.md` into the PR.
- Open a pull request; see [CONTRIBUTING.md](../CONTRIBUTING.md). Partial ports (say, display and touch only)
  are welcome. Mark how far you got.
