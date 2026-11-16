# M6e verification: rotation and Settings (Samsung Galaxy XCover Pro)

Status: 🟨 2026-11-16. Manual rotation, Settings and autorotate confirmed; SCREEN-mode landscape pending.

| Confirm item | Result |
|---|---|
| Every paneld mode usable in all 4 orientations | ✅ PANEL pages, MOUSE, KEYBOARD, COMBO, menu, Settings render rotated (screenshots); landscape keyboard taps type the right HID keys (`test-input-modes.py`) |
| Autorotate follows within ~1 s, no flapping at 45° | ✅ user-verified ("follows correctly"); the hub reports discrete orientations with its own hysteresis; flat = 255, ignored |
| Settings survive reboots; reachable with side buttons only | ✅ menu → SETTINGS via vol/XCover; values in the settings store |
| SCREEN (GUD) in landscape | ⬜ needs a 2340×1080 GUD mode with rotation in the blit |

## How autorotate works
- The sensor co-processor (Exynos CHUB) runs Samsung's SSP firmware, **built into the kernel**
  (`firmware/sensorhub/shub_bl.fw`, `shub_exynos9611_g715.fw`). Android powers it on with one sysfs write,
  and paneld now does the same: `echo 1 > /sys/devices/platform/11980000.contexthub/poweron`.
  The SSP driver then registers 21 IIO sensors.
- paneld enables **`device_orientation`** (type 27): computed on the co-processor, one event per change
  (0 upright, 1 left, 2 upside down, 3 right, 255 unknown/flat). As soon as it reports, the accelerometer
  stream (type 1, ~6 Hz, the fallback) is switched off, so **between rotations the main CPU does nothing**.
  Measured: 7 hub events during the user's test; accelerometer stopped after the first.
- Sensors run only while rotation = AUTO and the screen is on.
- Enable protocol: `echo $((type*10+1)) > /sys/class/sensors/ssp_sensor/enable`, then `in_timestamp_en=1`
  and `buffer/enable=1` on the IIO device; samples = payload + 8-byte timestamp.

## Implementation notes
- fb layer: panel size (`pw`/`ph`) vs logical size (`w`/`h`) + `rot`; `fb_fill` maps logical rects onto the
  panel, so all drawing code rotates unchanged. GUD, screenshots, raw touch and injection stay in panel coordinates.
- Touch: fingers in panel pixels; UI contacts and the corner long-press in logical pixels.
