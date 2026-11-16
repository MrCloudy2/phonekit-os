# M6b verification: phone modes (PANEL / SCREEN / MOUSE / KEYBOARD / COMBO)

Status: 🟨 2026-11-15. MOUSE, KEYBOARD, COMBO, mode menu and button switching confirmed with
`scripts/test-input-modes.py` (all pass). Pending: real-finger feel check by the user, PANEL power budget,
Windows/macOS hosts, WEBCAM research.

| Confirm item | Result |
|---|---|
| Trackpad moves the cursor with no host driver | ✅ Linux (HID mouse, relative 16-bit): swipe 400×400 px → 1166×1166 counts (with acceleration). Windows/macOS untested |
| Clicks and scrolling | ✅ tap = left, 2-finger tap = right, 3-finger tap = middle, 2-finger drag = wheel, bottom L/R zones hold buttons |
| Keyboard types into the host; modifiers work | ✅ KEY_A from the on-screen key; sticky Shift → Shift+A (HID boot keyboard, so it works in BIOS/UEFI too) |
| Combo (keyboard + trackpad strip) | ✅ strip clicks, keys type |
| Menu reachable with side buttons only | ✅ XCover long = menu, vol = move, XCover = select; XCover short = next mode |
| SCREEN picture survives visiting other modes | ✅ identical screenshots before/after (frames go to a shadow copy while hidden) |

Testing never touches the desktop: the phonepanel mouse and keyboard on the PC are grabbed exclusively
(`EVIOCGRAB`) while the test reads their events.

## Modes
| Mode | USB | Screen owner |
|---|---|---|
| PANEL | B (RNDIS network) | paneld pages (status now, widgets in M8) |
| SCREEN | A | host (GUD) + HID touchscreen |
| MOUSE | A | paneld trackpad UI → HID mouse |
| KEYBOARD | A | paneld keyboard UI → HID keyboard |
| COMBO | A | keyboard + trackpad strip |
Mode A always carries all three HID functions (touchscreen, mouse, keyboard) + GUD, so switching between
SCREEN/MOUSE/KEYBOARD/COMBO doesn't re-enumerate.

## Bugs found and fixed
- `mode N` passed the input-mode number to the menu, where 0 is PANEL: every check ran one mode off.
- Release frames weren't reported (no finger active), so taps and key presses, which complete on lift, never fired.
- **FunctionFS deadlock on mode switch** (phone went black, watchdog rebooted it): FunctionFS holds its mutex while a
  blocking ep0 `read()` waits for events, and `functionfs_unbind()` (UDC unbind) needs it. The ep0 thread now waits
  in `poll()` and reads non-blocking. 24 consecutive A↔B switches with GUD active: 0 failures.
- Host test picked the wrong "phonepanel" input device once mouse/keyboard existed (now matched by capabilities).
