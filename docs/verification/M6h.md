# M6h verification: DECK mode (Samsung Galaxy XCover Pro)

Status: ✅ 2026-11-17 (on-device button editing is a follow-up; the PC side and the store cover configuration).

## What was built
- `paneld/deck.c`: pages of 12 buttons in the PHONEBOX look (Roboto, rounded cards), 4×3 landscape / 3×4
  portrait, page tabs and USB/BT status in the header; combos held while pressed and merged across fingers,
  key sequences, typed text, media keys, toggles; `ppctl deck` to configure ([docs/deck.md](../deck.md)).
- USB: a fourth HID function (consumer control, `/dev/hidg3`); keyboard usages raised from 0x65 to 0xE7
  (F13–F24) in the USB and Bluetooth descriptors; Bluetooth consumer report 3.
- DECK, like RESCUE, leaves the GUD monitor out of the gadget: no phantom display on the PC.
- `tests/test_deck.c` in CI (ASan + UBSan).

| Check | Result |
|---|---|
| USB: buttons reach the PC (`ppctl deck press`, evdev grabbed) | ✅ F13 → KEY_F13, F17, play/pause, volume up/down: press + release each |
| USB: real taps on the phone | ✅ three taps = three F13 press/release; SCENE 2 + 3 held together = F14 + F15 down together, both released on lift |
| Bluetooth: media keys on an Android phone | ✅ volume ±, play/pause (after re-pairing for the new descriptor) |
| Pages, tabs, toggles, pressed state | ✅ by eye on the phone |
| Layout survives a reboot | ✅ custom button + page name kept; `deck reset all` removes the keys again |
| Latency < 50 ms | 🟨 not measured end to end; the report is written in the touch frame, before the button redraw |
| OBS switches a scene | ⬜ not tried here (any app taking F13 as a hotkey; KEY_F13 arrives as shown above) |

## A bug this found
The first DECK image hung the phone: `cv_rrect_stroke` (gfx.c) skips the inside of a box, and with a fractional
border width (DECK uses 2.4 px; PHONEBOX happened to use 1.8 px) the skip could land behind the current pixel and
loop forever. paneld stopped feeding the watchdog, which rebooted to the ROM as designed. Recovered by writing the
fixed image to the recovery partition from LineageOS over adb (store region kept, read back and verified). The skip
now only moves forwards, and `tests/test_deck.c` sweeps border widths 0.5–6 px under a timeout.
