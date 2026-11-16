# PhoneKit — Roadmap

> Turn any old phone with an unlockable bootloader into a quality touch panel for your
> projects. It works like [phonebox](https://github.com/ersinkoc/phonebox) (a desk panel with
> widgets driven by a host), but there's **no Android underneath**: just a trimmed kernel and
> one native daemon.

Name: **PhoneKit** (repo `phonekit-os`; renamed from PhoneKit 2026-11-16). Reference device: **Samsung Galaxy XCover Pro** (Exynos 9611,
removable battery, existing custom LineageOS build).

---

## 1. Product vision

| | phonebox | PhoneKit |
|---|---|---|
| OS on phone | Full Android 9+ | Trimmed vendor kernel + tiny initramfs |
| UI runtime | WebView + JS | Native renderer (LVGL candidate), drawing directly to DRM/fbdev |
| Boot to UI | Android boot (~30–60 s) | Target **< 5 s** |
| Host link | `adb reverse` / HTTPS over Wi-Fi | USB gadget (Mode A / Mode B) + Wi-Fi |
| Host types | PC (Win/Linux/macOS) | PC, SBC (Raspberry Pi etc.), **microcontrollers** (ESP32, Arduino, STM32) |
| Install | APK | Flash `boot.img` (per-device, from the device database) |

### Operating modes
- **Mode A: dumb display.** The phone is a USB monitor and touchscreen. It uses a GUD
  display gadget plus a HID multitouch digitizer. A Linux host needs no driver.
- **Mode B: smart display.** The phone renders widgets itself. The host (a PC agent, an SBC
  or a microcontroller) sends a layout plus data updates over USB-CDC serial, UART (OTG
  host plus a USB-serial adapter) or Wi-Fi. Touch events come back as widget events.
  This mode is the phonebox equivalent.

Both modes can be in one image and switch at runtime (see M5).

### Design principles
1. **Pixels first, everything else optional.** Boot straight to the display. Wi-Fi, OTA
   updates and widgets are layers on top.
2. **Draw only what changes.** Track damaged regions, send no frames when idle, dim and
   blank the screen aggressively. Low power is the default, not a setting.
3. **One daemon, no framework.** `paneld` (C or Rust) owns the display, input, transports
   and the widget engine.
4. **Ported devices are data, not code.** Each device is a record in the database, plus
   its kernel config fragment and panel quirks.
5. **Never cook a battery.** Charge limiting is a core feature, not an extra.

---

## 2. Architecture (target)

```
┌──────────────────────────── phone ────────────────────────────┐
│ trimmed vendor kernel (from LineageOS/TWRP tree)              │
│   DRM/KMS or fbdev · touch (evdev) · charger · USB UDC · WLAN │
│ initramfs                                                     │
│   busybox · paneld · (optional) wpa_supplicant + fw blobs     │
│                                                               │
│ paneld                                                        │
│   ├─ display backend   (KMS atomic / fbdev, damage tracking)  │
│   ├─ input backend     (evdev multitouch → gestures/events)   │
│   ├─ power manager     (backlight, DPMS, idle, charge limit)  │
│   ├─ widget engine     (layouts, data bindings, themes)       │
│   └─ transports                                               │
│        A: configfs gadget = GUD + HID digitizer (+ ACM)       │
│        B: CDC-ACM / NCM / UART-over-OTG / Wi-Fi (TLS)         │
└───────────────────────────────────────────────────────────────┘
          │ USB / Wi-Fi
┌──────── host ────────┐   ┌──── microcontroller ────┐
│ panel-agent (PC/SBC) │   │ libpanel (C, Arduino)   │
│ stats, media, hooks, │   │ tiny command protocol   │
│ layout editor        │   │ over UART / USB-CDC     │
└──────────────────────┘   └─────────────────────────┘
```

---

## 3. Milestones

Each milestone has **Deliverables** and **Confirm** criteria. A milestone is done only when
every Confirm item has been checked on real hardware (XCover Pro unless noted) and the
result is recorded in `docs/verification/Mxx.md`.

Status legend: ⬜ not started · 🟨 in progress · ✅ confirmed

---

### Phase 0: Foundations

#### M0 · Project setup ✅
*Confirmed 2026-11-15 on the XCover Pro (`docs/verification/M0.md`). The image goes on the
**recovery** partition so LineageOS on `boot` stays untouched. The M0 stub already draws
pixels (kernel → pixels 2.48 s) and paints touch, so M1 and M2 have a head start.*

**Deliverables**
- Repo layout: `paneld/`, `initramfs/`, `devices/`, `host/`, `protocol/`, `docs/`.
- Build script that produces a `boot.img` from an existing kernel `Image` + DTB + our
  initramfs (mkbootimg, Samsung header handling, AVB/vbmeta notes).
- Cross toolchain (aarch64, static musl).

**Confirm**
- [ ] `make device=samsung-xcoverpro` builds a flashable `boot.img` from a clean checkout.
- [ ] Flashing that image and then the stock LineageOS boot image round-trips cleanly (no brick, recovery reachable).

#### M1 · Boot to pixels 🟨
*In progress (`docs/verification/M1.md`). Done: USB-ACM control channel, hands-free adb deploy loop,
software watchdog. Measured bootloader 4.16 s + kernel→pixels 2.48 s ≈ 6.6 s, so the < 5 s target is
out of reach while the Samsung bootloader alone takes 4.16 s. Likely re-target: kernel→pixels < 1.5 s.*

**Deliverables**
- Kernel config fragment that disables what isn't needed (modem/RIL, camera, audio, NFC,
  sensors, binder/Android-only bits where possible).
- initramfs with busybox and a static `paneld` that draws a test pattern.
- Serial/USB-ACM console for debugging.

**Confirm**
- [ ] Phone boots into the test pattern with no Android userspace present.
- [ ] Power-on to first pixel **< 5 s**, measured.
- [ ] A shell is reachable over USB-ACM.
- [ ] Watchdog is satisfied: the phone stays up for 24 h without rebooting.
- [ ] Record which display API works (KMS atomic / legacy KMS / fbdev) in the device record.

#### M2 · Touch input ✅
*Confirmed 2026-11-15 (`docs/verification/M2.md`, `scripts/test-touch.py`): 10-finger tracking, 4 rotations,
software latency avg 28 ms, glove mode.*

**Deliverables**
- evdev multitouch reader in `paneld`, with rotation and calibration mapping.
- Touch-paint debug app.

**Confirm**
- [ ] All supported touch points tracked; coordinates correct in all 4 rotations.
- [ ] Touch-to-photon latency measured (high-speed camera or LED method); target **< 50 ms** locally.
- [ ] Gloved/wet touch modes (XCover-specific) still work or are documented as lost.

---

### Phase 1: Power and battery

#### M3 · Low-power display pipeline ✅
*Confirmed 2026-11-15 (`docs/verification/M3.md`): damage tracking, zero flips when idle, paneld < 0.01 % CPU,
power table via fuel gauge in slate mode. Video-mode panel: refresh-rate reduction limited to ~54 Hz by
porch; DSI-clock experiment deferred.*

**Deliverables**
- Damage tracking: only changed rectangles get rendered or flushed.
- Idle detection: page flips stop entirely when the screen is static.
- Command-mode panel self-refresh / partial update where the panel supports it.
- Frame pacing: dynamic update rate (e.g. 60 fps during animation or touch, down to 0 when idle).

**Confirm**
- [ ] With a static screen, CPU stays in its lowest idle state (verify with `/sys` cpuidle stats); `paneld` CPU < 1 %.
- [ ] A 1 Hz clock widget redraws only its own rectangle (visible in a debug overlay).
- [ ] Power draw measured with a USB power meter, battery removed if the device allows it, at: static screen at 50 % brightness, screen blanked, active animation. Numbers go in the device record.

#### M4 · Power manager and battery care 🟨
*Functional checks confirmed 2026-11-15 (`docs/verification/M4.md`): dim/off states, 263 ms wake, power key
toggle/hold-reboot, charge limiter (battery 0 µA when held), thermal guard. Pending: 72 h window soak,
battery-less test, < 100 mW off (133 mW now; needs suspend-to-RAM).*

**Deliverables**
- Backlight control, auto-dim, DPMS blank on idle, wake on touch or on host command.
- CPU governor / hotplug policy tuned for "mostly idle".
- **Charge limiter**: hold the battery between configurable levels (default 40–60 %), using
  kernel charger control or input-current limiting where available.
- Thermal monitoring and shutdown; battery temperature reported to the host.
- Battery-less operation documented per device.

**Confirm**
- [ ] Plugged in for 72 h, the battery stays inside the configured window.
- [ ] Battery temperature never exceeds the configured limit; the overheat path is tested by lowering the limit.
- [ ] Blank → wake on touch in **< 300 ms**.
- [ ] Idle power with the screen blanked is measured and recorded (target: **< 100 mW**, to verify).
- [ ] Clean behaviour on power loss (no filesystem corruption: rootfs is read-only and config uses atomic writes).

#### M4b · Standby policy: what the phone does when the host is gone 🟨
*2026-11-16: hold / ambient / sleep done in PHONEBOX mode and in Settings (`docs/verification/M4b.md`); PC-suspend test and the PPJ/1 path pending.*
*Added 2026-11-16 at the user's request.* A Settings entry, per mode, the user chooses:
**Deliverables**
- **Ambient**: clock + user-picked basic widgets (date, battery, last values), refreshed once a minute, CPUs
  capped. Note: on LCD phones the backlight dominates (~300+ mW); OLED phones do this far cheaper.
- **Hold**: keep the last panel, dimmed, with an "offline" badge.
- **Sleep**: screen off; wake automatically on host activity: USB resume (the gadget sees the host's
  suspend/resume, so "PC sleeps → panel sleeps, PC wakes → panel wakes" needs no host software), a new
  connection, incoming data, or a key.
- **Off**: power off until cable plug-in or the power key.
- Later: real suspend-to-RAM for Sleep (see stretch goal "Deep standby").

**Confirm**
- [ ] Each choice survives a reboot and behaves as described when the host disconnects / sleeps.
- [ ] Host suspend → phone sleeps within 5 s; host resume → panel back within 1 s.
- [ ] Ambient power measured and recorded.

---

### Phase 2: USB communication (Mode A / Mode B)

#### M5 · USB gadget framework and mode switching ✅
*Confirmed 2026-11-15 (`docs/verification/M5.md`): runtime A↔B switching, 50/50 replugs, settings store,
RNDIS network for mode B (Samsung NCM can't transmit), HID touchscreen in mode A, configurable side buttons.*

**Deliverables**
- configfs gadget manager in `paneld`: builds composite gadgets at runtime.
- Profiles: **A** = GUD + HID digitizer + ACM (console). **B** = ACM (protocol) or NCM (IP) + ACM (console).
- Mode selection: persisted setting, on-screen menu (long-press corner), host command.
- Fallback: if no host enumerates the gadget within N s, show a "waiting for host" screen with the mode and device info.

**Confirm**
- [ ] Switching A ↔ B at runtime re-enumerates on the host without rebooting the phone.
- [ ] Unplug and replug in either mode recovers automatically, 50 times in a row.
- [ ] The console ACM is present in both modes.

#### M6 · Mode A: USB monitor and touchscreen ✅
*Confirmed 2026-11-15 (`docs/verification/M6.md`): GUD over FunctionFS + HID touch, KDE/Wayland output with
touch mapped, 53 fps full-frame (30 fps USB-bound worst case), idle = 0 frames. Not tested: Raspberry Pi, X11.*

**Deliverables**
- GUD gadget (FunctionFS userspace implementation, based on samcday/gud-gadget, or a
  kernel module) feeding into `paneld`'s display backend.
- HID multitouch digitizer gadget whose report descriptor matches the panel resolution.
- Host-side notes for Linux (GUD has been in mainline since 5.13). Windows/macOS: document the
  limitation and evaluate a userspace host shim later.

**Confirm**
- [ ] A Linux PC and a Raspberry Pi both see a new monitor and touchscreen with no extra install.
- [ ] 60 fps at native resolution with a compressed/RGB565 path over USB 2.0, measured.
- [ ] Touch maps 1:1 to the correct monitor on a multi-monitor host (X11 and Wayland).
- [ ] Idle host desktop → the phone is in M3 idle (no flushes).

#### M7 · Mode B: smart display protocol ✅
**Deliverables**
- `protocol/` spec v1: framed binary messages with a text/JSON debug variant. Covers
  layout, widget create/update/delete, data binding updates, touch/widget events, ping,
  device info and capabilities.
- Transports: USB-CDC ACM, USB NCM (TCP), UART via OTG host plus a CH340/CP210x/FTDI adapter.
- `libpanel` reference client in C (portable, Arduino-compatible) and Python.
- Flow control and backpressure; message size limits suitable for small MCUs.

**Confirm**
- [ ] An ESP32 (UART via OTG adapter) builds a 3-widget screen and receives button events.
- [ ] An Arduino Uno-class MCU (2 KB RAM) can drive a basic screen with libpanel.
- [ ] A Python client on a PC drives the same screen over USB-CDC and over NCM.
- [ ] Protocol fuzzing (malformed frames) cannot crash `paneld`.
- [ ] Update latency (host sends a value → pixels change) is **< 30 ms** over USB.

---

#### M6b · Phone modes: PANEL, SCREEN, MOUSE, KEYBOARD, COMBO (+ WEBCAM research) 🟨
*User's mode list (2026-11-15), switchable with the side buttons (XCover short = next mode, XCover long =
menu). PANEL = phonebox-style dashboard on USB B; the others share USB A without re-enumerating.*
- **PANEL-mode power budget**: PANEL is where the phone spends ~99 % of its time, so nothing may run in the
  background there. Confirm: CPU idle ≥ 99 %, no wakeups apart from widget updates, power measured per state.
- **WEBCAM (research)**: UVC gadget is standard. Getting frames from Samsung's fimc-is camera stack without the
  Android camera HAL may need heavy reverse-engineering. Feasibility study first.

*Added 2026-11-15 at the user's request.*
**Deliverables**
- On-screen **mode menu** (opened with a side button): touchscreen (HID digitizer) · trackpad · keyboard · smart display.
- **Trackpad mode**: phone screen as a PC touchpad. Relative HID mouse (move, tap = click, two-finger tap =
  right click, two-finger drag = scroll); later a Windows Precision Touchpad descriptor.
- **Keyboard mode**: on-screen keyboard rendered by paneld, HID boot keyboard reports; layouts; macro/shortcut keys.
- Several HID functions in one gadget, so modes switch without re-enumerating where possible.

**Confirm**
- [ ] Trackpad moves the cursor smoothly on Linux, Windows and macOS with no host driver.
- [ ] Keyboard types into a host text field; modifiers and shortcuts work.
- [ ] Menu reachable with side buttons only (no touch needed to switch out of keyboard mode).

#### M6e · Rotation and on-device Settings 🟨
*2026-11-15: manual rotation for all paneld screens (logical vs panel coordinates in the fb layer), Settings
screen (rotation, brightness, dim/off, charge window, USB network), all persisted; landscape keyboard verified
with a HID test. Next: autorotate (sensor hub CHUB needs firmware from the vendor partition + `poweron`),
SCREEN landscape (2340×1080 GUD mode).*

*Added 2026-11-15 at the user's request.*
**Deliverables**
- **Settings screen** (from the mode menu); first entry **Rotation: Auto / Portrait / Landscape / Portrait-flipped /
  Landscape-flipped**, then brightness, dim/off timeouts, charge window, USB network type, button bindings.
  All values in the settings store.
- **Landscape for every mode**: PANEL, MOUSE, KEYBOARD and COMBO draw through the logical↔panel transform
  (verified in M2); landscape layouts for the keyboard/combo.
- **SCREEN landscape**: offer the host both 1080×2340 and 2340×1080 modes; the phone rotates pixels while converting.
- **Autorotate**: read the accelerometer behind Samsung's SSP sensor hub without Android (interface to be found),
  with hysteresis and a lock.

**Confirm**
- [ ] Every mode usable in all 4 orientations; touch and HID output stay correct after rotating.
- [ ] Autorotate follows the phone within ~1 s and doesn't flap at 45°.
- [ ] Settings survive reboots; the menu is reachable with side buttons only.

#### M6c · Side buttons and on-device navigation 🟨
*Started 2026-11-15 (`paneld/keys.c`).* Done: short/long-press actions per key, bound in the settings store,
default XCover layout, wake-only press when off. Next: page navigation for the M8 widget pages, a settings
menu on-device, per-page key bindings.

#### M6d · Mass storage: flash drive and portable debug device ⬜
*Added 2026-11-15 at the user's request.*
**Deliverables**
- USB mass-storage function (kernel option enabled 2026-11-15) alongside the other modes, in any mode.
- Backing store, in order of preference: **microSD card** (XCover Pro slot; nothing of the ROM lives there),
  an image file on the card, optionally a RAM disk for scratch use. Phone-internal partitions stay off
  limits by default (`cache` is used by LineageOS).
- **CD-ROM emulation of a bootable ISO** picked on the phone screen (Ventoy-style rescue/installer stick).
- On-screen file/ISO picker; read-only toggle; "eject" from the phone.
- Later: phone as a debug console for target boards (USB-serial adapters over OTG host, see M7).

**Confirm**
- [ ] PC mounts the drive; 1 GB copy round-trip with matching checksums; sustained speed recorded.
- [ ] A PC boots from an ISO exposed as CD-ROM.
- [ ] Switching ISO / ejecting from the phone works without re-plugging.

#### M6f · Install layouts and maintenance menu ⬜
*Added 2026-11-16.* Recovery stays the default target (it keeps Android as a working fallback), but it is a choice.
**Deliverables**
- Documented layouts: **dual** (recovery: power on → ROM, combo → PhoneKit), **dedicated** (boot: straight to
  PhoneKit, no ROM), **A/B slot** (devices without a recovery partition: spare slot + `fastboot set_active`).
- README note for technical users: in the dual layout the ROM's own recovery is gone, so **ROM OTA updates that
  install through recovery stop working**. `make restore-recovery` puts the ROM's recovery back for an update.
- On-device maintenance menu: reboot to download/fastboot/ROM, expose a partition read-only over USB mass
  storage for backup, wipe `/data` (with confirmation), version info, shell. No OTA installer (out of scope).

**Confirm**
- [ ] Each layout installed and reverted on at least one device; ROM OTA works after `restore-recovery`.
- [ ] Partition backup over USB matches the on-device sha256.

#### M6g · RESCUE mode: console for headless servers 🟨
*2026-11-16: terminal + on-screen keyboard on the console port work (`docs/rescue.md`, `docs/verification/M6g.md`); bootstrap macro and SSH-over-USB next.*
*Added 2026-11-16.* The phone as a crash cart for a server that lost its network.
**Deliverables**
- Composite gadget: **HID keyboard + CDC-ACM serial + USB network** at once. CDC-ACM and RNDIS/NCM are in
  every distro kernel, no GUD needed.
- On-phone **terminal** (VT100/xterm subset renderer in paneld) on the serial port, with the COMBO keyboard.
- **Bootstrap macro** for unprepared servers: types blind on the server's own console (login, then
  `systemctl start serial-getty@ttyACM0`, or `dhclient usb0`), after which the output shows on the phone.
  Credentials are typed from the phone's on-screen keyboard and never stored unless the user opts in.
- Docs: one-time server prep (`systemctl enable serial-getty@ttyACM0` or a udev rule) so plugging in shows a
  login prompt immediately; optional GUD console (`gud` is in mainline ≥ 5.13; fbcon only lands on it if it's the first framebuffer).
- Known limit: BIOS/GRUB/an unbootable server are **keyboard only** (no OS → no serial; the phone can't
  capture HDMI because its one USB port is in device mode).

**Confirm**
- [ ] Prepared server: plug in → login prompt on the phone in < 5 s; full-screen apps (htop, vim) render.
- [ ] Unprepared server with a console login: the bootstrap macro gets a shell on the phone.
- [ ] SSH over the USB link to the phone's DHCP address while the server's main network is down.

### Phase 3: Widgets in the image

#### M8 · Widget engine ⬜
**Deliverables**
- Widget engine in `paneld`. Evaluate LVGL (DRM/fbdev + evdev drivers, dirty-area
  rendering, small footprint) against a custom engine.
- Grid layout model (phonebox-style 12×6, multiple pages, swipe between pages).
- Built-in widgets: label/value, gauge, bar, sparkline/graph, button, toggle, slider,
  image, clock, progress, list, notification toast.
- Theme system: colours, fonts, dark/light, per-page backgrounds.
- Layouts stored on the device and loadable from the host.

**Confirm**
- [ ] Every built-in widget renders correctly and respects damage tracking (M3 criteria still pass with a full dashboard).
- [ ] A 4-page dashboard with live graphs keeps **≥ 50 fps** during swipes.
- [ ] RAM usage of `paneld` with a full dashboard stays **< 64 MB**.
- [ ] Layout survives a reboot and appears **< 5 s** after power-on with the last-known data.

#### M9 · Standalone widgets (no host required) ⬜
**Deliverables**
- Widgets that work offline from on-device data: clock, battery, uptime, local sensors if available.
- Optional (once Wi-Fi works): weather, RSS/HN, exchange rates, Home Assistant entities.

**Confirm**
- [ ] With no host connected, the phone shows a useful clock/status page indefinitely.
- [ ] Network widgets degrade gracefully when offline (show the last value and stale state).

---

### Phase 4: Wireless

#### M10 · Wi-Fi bring-up ⬜
**Deliverables**
- Load WLAN firmware from the vendor partition (mounted read-only) or a bundled blob, per device.
- wpa_supplicant (or iwd) in the initramfs; on-screen Wi-Fi setup plus provisioning from the host over USB.
- mDNS advertisement (`_phonepanel._tcp`).

**Confirm**
- [ ] Joins WPA2/WPA3 networks; reconnects after a router reboot without user action.
- [ ] Wi-Fi power-save enabled; idle power with Wi-Fi connected is measured and recorded.
- [ ] Discoverable from the host via mDNS within 10 s of boot.

#### M11 · Secure pairing and Mode B over Wi-Fi ⬜
**Deliverables**
- QR-code pairing shown on the panel; pinned TLS plus a shared key (same model as phonebox).
- The Mode B protocol over TCP/TLS (and optionally WebSocket for browser clients).

**Confirm**
- [ ] Pairing works with no cable.
- [ ] Unpaired clients are rejected; a replayed or tampered session fails.
- [ ] Same latency targets as M7 on a local LAN (**< 50 ms**).

#### M10b · Tethering: phone as a USB Wi-Fi / mobile-data adapter ⬜
*Added 2026-11-16.*
**Deliverables**
- The USB network function (RNDIS/NCM, driverless on Linux/Windows/macOS) routed (NAT) to the phone's Wi-Fi
  or cellular uplink, so the host gets internet. Can run **alongside** other modes (composite gadget).
- Cellular is SoC-dependent: Qualcomm modems work with ModemManager/QMI (postmarketOS proves it); Samsung
  Exynos Shannon modems need Android's closed RIL, so likely not feasible there. Status per device in `device.yaml`.
- Optional Wi-Fi hotspot (AP mode) where the driver supports it.
- Note in the docs: some carrier plans restrict tethering.

**Confirm**
- [ ] Host browses via the phone's Wi-Fi with no driver install on Linux, Windows and macOS; throughput recorded (USB 2.0 caps ~300 Mbit/s).
- [ ] Tethering runs together with PANEL or COMBO mode.
- [ ] (Qualcomm device) same over mobile data.

#### M10c · Bluetooth and NFC (optional per device) ⬜
*Added 2026-11-16.* Each porter decides which radios to bring up; status lives in `device.yaml`.
**Deliverables**
- Bluetooth via BlueZ where the kernel driver exists (XCover: Samsung SCSC, same chip as Wi-Fi).
- **Phone as a Bluetooth keyboard/trackpad** (HID device profile): wireless MOUSE/KEYBOARD/COMBO for any host,
  including tablets and TVs. BLE transport for MCU projects (stretch goal moves here).
- NFC: lowest priority. Samsung NFC is tied to the secure element (disabling it hung boot, M1) and needs a
  userspace NCI stack port.

**Confirm**
- [ ] Pairs with a Linux, Windows and Android host as a keyboard + trackpad; reconnects after reboot.
- [ ] Idle power with Bluetooth on is measured.

#### M11b · Remote console passthrough ⬜
*Added 2026-11-16. Needs M6g + M11.* The phone sits on a server by USB (keyboard + serial) and on your network
by Wi-Fi, and bridges them: a pocket serial-console server / PiKVM-lite.
**Deliverables**
- SSH server on the phone (dropbear, key-only) that attaches to the server's serial console and HID keyboard.
- Pairing and keys from M11; nothing listens unpaired; on-screen indicator + log while a remote session is active.
- Optional web terminal for the host agent.

**Confirm**
- [ ] From another PC on the LAN: shell on the server through the phone; works while the server's own network is down.
- [ ] Unpaired/unknown keys are rejected; remote access can be switched off on the phone.

---

### Phase 5: Seamless host integration (the phonebox experience)

#### M12 · Host agent ⬜
**Deliverables**
- `panel-agent` for Linux, Windows and macOS (single static binary; Rust or Go candidate).
- Auto-detects the panel over USB (Mode B) and the LAN (mDNS); zero-config on USB.
- Data providers: CPU, GPU, RAM, disks, network, media/now-playing (MPRIS / Windows SMTC /
  macOS), volume, shortcuts/macros (Stream Deck style), webhooks for scripts, CI and
  Home Assistant, and rules (switch page on a condition).
- Pushes layouts and data to the panel; the panel renders natively (no web stack on the phone).

**Confirm**
- [ ] Install agent → plug in phone → the default dashboard appears with live stats in **< 10 s** and no configuration.
- [ ] Works on Linux, Windows and macOS (Mode B over USB-CDC needs no driver on any of them).
- [ ] A shortcut button on the phone triggers a host action in **< 100 ms**.
- [ ] A webhook `curl` updates a widget value.
- [ ] Agent restart or phone replug: everything recovers without user action.

#### M12b · PHONEBOX mode: phonebox desktop compatibility 🟨
*Started 2026-11-16 (`docs/verification/M12b.md`). The user's call: PANEL mode becomes PHONEBOX mode, so we don't
need our own host app.* Approach: **the phone pretends to be a phone with the phonebox app**, so the stock agent
works unmodified. (A bridge in our own host agent, the first plan, isn't needed.)
**Deliverables**
- ✅ Minimal adbd (`paneld/adbd.c`, FunctionFS): devices/shell/exec, the agent's Android commands emulated
  (`pm path`, `am start`, `input keyevent`, `settings`, `cmd package`), `adb reverse`, `root`, `reboot`.
  USB ID 18d1:4ee4 in PHONEBOX mode so stock udev rules give adb access.
- ✅ Native client (`paneld/pbclient.c`): `/api/config` pages → native widgets, `/api/stream` state → values;
  taps → the same POSTs the app sends (`/api/run/<id>`, `/api/media/<action>`). Portrait transposes the
  landscape 12×6 grid; rotation re-flows.
- ⬜ More widget types (table in the verification doc), page switching from the agent (`goto` events),
  notifications, approvals, the agent's Wi-Fi pairing route (M11), RSA adb auth (optional).

**Confirm**
- [x] An existing phonebox dashboard shows up on PhoneKit without editing it (stock agent 0.9.14, default pages).
- [x] Live values update; buttons send the same requests as the phonebox app (`scripts/test-phonebox.py`).
- [ ] Every default-template widget type drawn natively.
- [x] `README.md` describes the setup.

#### M13 · Layout editor ⬜
**Deliverables**
- Browser-based editor served by the agent: drag widgets onto the grid, bind data sources, set themes.
- Live preview on the phone while editing.
- Import/export of layouts as JSON; evaluate importing phonebox `config.json` pages
  (phonebox is MIT licensed).

**Confirm**
- [ ] A non-developer builds a 2-page dashboard without editing JSON.
- [ ] Edits appear on the phone in **< 1 s**.

#### M14 · Updates and recovery ⬜
**Deliverables**
- Update `paneld`, the initramfs or the whole boot image from the agent. Use A/B slots where the device has them, and keep a fallback image.
- Safe mode: hold a button combo or touch pattern at boot → console plus default layout.

**Confirm**
- [ ] A failed update (power pulled mid-flash) boots back into the previous working version.
- [ ] Version is visible on the panel and in the agent.

---

### Phase 6: Device database and porting skill

#### M15 · Device database v1 ⬜
**Deliverables**
- `devices/<vendor>-<codename>/device.yaml` schema:
  - identity: name, codename, SoC, RAM, panel resolution/size/type, touch controller
  - boot: unlock method, flash tool (Odin/heimdall/fastboot/mtk), boot image header version, AVB/vbmeta handling, A/B slots
  - kernel: source repo/branch (LineageOS/TWRP/pmOS), defconfig, our config fragment, DTB/DTBO
  - display: API (KMS/fbdev), video vs command mode panel, max refresh, partial update support
  - input: touch node, max points, special modes (glove/wet)
  - USB: UDC name, gadget/configfs support, OTG host capable, USB speed
  - power: charger control path, charge-limit method, battery-less boot (y/n), measured power numbers
  - wifi: chip, firmware location, status
  - status per milestone (M1…M14), tester, date
- Generated compatibility table (`docs/devices.md`).

**Confirm**
- [ ] The XCover Pro record is complete and every claim in it is verified.
- [ ] Records for 2 more devices from different SoC families (e.g. Qualcomm and MediaTek), at least at M2.
- [ ] A schema validator runs in CI.

#### M15b · pptest: hardware test kit for ports 🟨
*2026-11-16: `scripts/pptest.py` runs the suites and writes `docs/verification/<device>-report.md`; stress/soak/fuzz-in-CI next.*
*Added 2026-11-16.* So self-written (or AI-written) drivers and ports are proven, not assumed.
**Deliverables**
- `pptest`: reads a device's claimed capabilities from `device.yaml`, runs the matching suites
  (today's `scripts/test-*.py` become plugins), and writes `docs/verification/<device>.md` for the PR.
- Stress and soak: 100× reboot loop, USB replug and mode-switch loops, 24/72 h soak tracking paneld RSS and fd
  count, thermal soak with charge-limit checks, fault injection (unplug mid-transfer, key spam, low battery).
- Host-side fuzzing of every parser (PPJ/1 JSON, ctl channel, GUD requests) with libFuzzer/AFL, and
  ASan/UBSan builds under qemu.
- Debug kernel fragment (lockdep, KASAN where supported) for test builds; crash logs (`last_kmsg`, watchdog
  dumps) collected into the report automatically.
- CI on every PR: build + qemu + fuzz smoke; hardware results come from the PR template.
- PROMPT.md/AGENTS.md: an agent may not claim a capability without its suite output.

**Confirm**
- [ ] The XCover Pro passes the full kit; the report is generated, not hand-written.
- [ ] A deliberately broken build (e.g. leaking fds, mishandled replug) is caught by the kit.

#### M16 · Porting skill ⬜
**Deliverables**
- A Claude skill (plus a plain-text porting guide) that takes a phone model and:
  checks unlockability and existing kernel trees (LineageOS, TWRP, postmarketOS), drafts
  `device.yaml`, builds the kernel config fragment, produces `boot.img`, and walks the
  user through the flashing and confirmation checklist for M1–M6.
- Probe script that runs on the phone (in our initramfs or in TWRP) and auto-fills the
  hardware fields: DRM/fbdev, input nodes, UDC, charger sysfs paths.

**Confirm**
- [ ] Someone other than the author ports a new device to M2 using only the skill and guide.
- [ ] The probe script output fills in ≥ 70 % of a new `device.yaml` automatically.

#### M17 · Public release v1.0 ⬜
**Confirm**
- [ ] ≥ 5 devices at M8 or higher in the database.
- [ ] Docs: quick start, porting guide, protocol spec, MCU examples, safety notes (batteries!).
- [ ] Prebuilt images plus checksums for every supported device.

---

*M17 release checklist additions (2026-11-16):* rename decided; demo video/GIF; prebuilt XCover image;
ideally a second device. Where to announce: XDA (general + per-device), Show HN, Lobsters, r/linux,
r/embedded, r/homelab, r/selfhosted, r/homeassistant, r/LineageOS, postmarketOS community, r/righttorepair,
Hackaday tips, Mastodon.

## 4. Stretch goals
- **Deep standby**: suspend-to-RAM when the screen is off (133 mW now, phones idle at ~10–20 mW suspended);
  needs a wake strategy for the USB host (M4 notes).
- **Low-refresh idle** on video-mode panels by lowering the DSI clock (M3 notes; porch-only limit is ~54 Hz).
- Windows/macOS Mode A via a userspace host shim (GUD isn't available there).
- Mainline kernel path for devices with good mainline support (via postmarketOS), for longevity.
- Haptics, the phone's own sensors (light sensor for auto-brightness, accelerometer for auto-rotate) exposed as widgets or events.
- Multiple panels driven by one agent (multi-panel desks).
- Audio out (notification sounds) via ALSA.

## 5. Key risks
| Risk | Impact | Mitigation |
|---|---|---|
| Downstream display stack needs Android blobs (HWC) | No pixels without Android | Recovery/TWRP proves a minimal display path; prefer devices with TWRP |
| The GUD gadget isn't mainlined | Mode A maintenance | Use a FunctionFS userspace implementation, no kernel module |
| Wi-Fi firmware and driver complexity | M10 slips | USB-first design; Wi-Fi is optional per device |
| Battery swelling in always-on installs | Safety | M4 charge limiter is mandatory before any "always plugged" use |
| Per-device porting effort | Slow database growth | Probe script + skill (M16); prioritise devices with existing LineageOS/TWRP trees |
| Samsung-specific boot quirks (vbmeta, Odin, Knox) | Brick risk | Document per device; always keep a known-good stock/LOS image |

## 6. Open questions
- Internal renames left from phonepanel (do together with the next on-phone test, since they change what the host sees):
  USB manufacturer string and `ppctl.py` port matching, KWin's saved touchscreen→monitor mapping (keyed by
  device name), image name `phonepanel.img`, kernel fragment `phonepanel.cfg`, mDNS `_phonepanel._tcp`, boot
  splash text. `paneld` keeps its name.
- Name: **PhoneKit**, repo `phonekit-os` (decided 2026-11-16; the GitHub org, npm package and phonekit.io/.com belong to unrelated products).
- Renderer: LVGL (C, mature) vs Slint (Rust) vs custom. Decide at the start of M8, after M3 power measurements.
- `paneld` language: C vs Rust. Leaning Rust for the protocol and transports, with a C ABI for LVGL.
- Protocol encoding: CBOR vs a custom TLV for MCU friendliness.
- Should Mode A and Mode B run at the same time (monitor + widget overlay)?
