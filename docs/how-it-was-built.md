# How the first port was built (Samsung Galaxy XCover Pro)

This is the path the first port took, mistakes included, so you know what porting a phone really involves.
Each step has a verification record in [verification/](verification/). The port was done by one person and an AI coding
assistant (Claude Code). The assistant did the code, builds and log reading; the human did the buttons and
the cables, and looked at the screen when a camera couldn't.

## 0. Starting point
- A Samsung XCover Pro (Exynos 9611), bootloader already unlocked, running a self-built **LineageOS
  userdebug** ROM. You don't strictly need your own build, but a userdebug ROM with "rooted debugging" makes
  everything after this much easier (see step 2).
- The LineageOS `recovery.img` from that build: a known-good image containing a kernel that
  initialises the display without Android. This is the single most important ingredient. **If TWRP or a
  LineageOS recovery shows a picture on your phone, phonepanel can too.**

## 1. Boot to pixels (M0)
- Unpack the known-good recovery image (`unpack_bootimg`). Keep its kernel, DTB and DTBO byte for byte;
  replace only the ramdisk with our own: `/init` is a static aarch64 `paneld`.
- Repack with the same header version (v2), page size and offsets, and append an AVB hash footer so the
  image is exactly partition-sized (`scripts/mkimage.sh`). Builds are reproducible (fixed AVB salt, mtime-0
  cpio, `gzip -n`).
- Flash to **recovery**, not boot. LineageOS stays on boot, so the phone always has a working OS.
- First surprise: LineageOS put its own recovery back on every boot (`persist.vendor.recovery_update=true`).
  Setting it to false fixed that.
- Result: a test pattern via fbdev 2.45 s after the kernel started, and touch painting under the finger.

## 2. A loop that doesn't need hands (M1)
Pressing button combos for every test is slow and wears you out. So:
- `make deploy`: `adb root`, `dd` the image onto the recovery partition, verify sha256, `adb reboot recovery`.
- paneld brings up a USB gadget with serial ports: a **control channel** (`scripts/ppctl.py`: info,
  screenshots, touch injection, file transfer), a busybox **shell**, and a read-only log port.
- A **watchdog** thread: if paneld's main loop is stuck for 15 s, it dumps the kernel stack to the log and
  reboots into LineageOS. `make logs` then reads it from `/proc/last_kmsg`.
- Surprise: the main loop froze. The cause wasn't the phone at all: a program on the PC polled
  `/dev/ttyACM0` at 100 Hz and wrote binary frames into the control port. The fix was to ignore anything that
  isn't a command, and to make ACM0 a harmless log-only decoy port. **Lesson: suspect the host too.**

## 3. Building the kernel from source (M1)
- The ROM's kernel tree builds with a modern host clang after two small patches
  (`devices/samsung-xcoverpro/kernel-patches/`: OpenSSL 3+/4 removed the ENGINE API; a compiler banner with
  two lines broke `mkcompile_h`).
- `initcall_debug` showed where boot time went. The audio codec probe alone took 390 ms, so it was disabled in a
  small config fragment (`kernel/phonepanel.cfg`). Kernel→pixels dropped to 2.06 s.
- Surprise: disabling NFC/eSE hung the kernel before the display came up. No log, back to download
  mode. The likely cause is that the secure world owns that SPI bus. **Lesson: trim little, measure first,
  and keep a known-good image ready for download mode** (see [recovery.md](recovery.md)).

## 4. Touch, power, battery (M2–M4)
- Touch: evdev multitouch, 10 fingers, 28 ms touch→pixel.
- Power: only redraw what changed (damage tracking), no frames when idle. Measured on the fuel gauge:
  728 mW active, 380 mW dimmed, 133 mW screen off.
- Battery: the Samsung charger driver exposes `charging_enabled`. paneld holds the battery between 40 % and
  60 % and stops charging above 45 °C. An always-plugged phone must have this.

## 5. USB modes (M5–M6b)
- configfs gadget with switchable functions:
  - **SCREEN**: a GUD display (Linux `gud` driver) implemented in userspace over FunctionFS, LZ4 compressed,
    plus a HID multitouch digitizer. ~53 fps full screen.
  - **MOUSE / KEYBOARD / COMBO**: plain HID functions.
  - **PANEL**: RNDIS network with a tiny DHCP server (phone 10.42.0.2, host 10.42.0.1). Samsung's NCM didn't
    transmit on this kernel.
- Surprises:
  - The colours were swapped. The Samsung DECON driver hard-codes ABGR, so fbdev's own report was wrong. This is
    now a per-device quirk, `FB_SWAP_RB`.
  - Unbinding the UDC deadlocked while a FunctionFS endpoint read was pending. It was fixed by polling ep0.
  - After a long screen-off, the picture never came back. DECON's pan doesn't re-enable the window after a
    powerdown, so paneld now forces `FBIOPUT_VSCREENINFO` after unblank.

## 6. Rotation and Settings (M6e)
- The phone's sensor co-processor (CHUB) runs Samsung firmware that is **built into the kernel**. One sysfs
  write powers it on, and it reports `device_orientation` events computed on the co-processor. So autorotate
  costs the main CPU nothing between rotations.

## 7. What's next
The PANEL widget protocol ([PPJ/1](../protocol/PPJ1.md)), phonebox compatibility, Wi-Fi, a host agent, and,
most importantly for you, more devices. See [ROADMAP.md](../ROADMAP.md).

## What this means for your phone
Most of the work above is **done once**. paneld, the build scripts, the USB modes and the protocol are shared.
A new port mostly has to:
1. Get a picture from a known-good kernel (steps 1–2): a day if TWRP/LineageOS exists.
2. Find that device's quirks (colour order, pan behaviour, touch node, charger path, buttons) and write them
   down in `device.yaml` / `device.mk`.
3. Optionally build the kernel from source for trimming and extra gadget functions.

Phones on the same SoC share most quirks; that's what [`socs/`](../socs/) is for.
