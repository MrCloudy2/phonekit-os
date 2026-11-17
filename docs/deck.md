# DECK mode: the phone as a hotkey pad

DECK turns the phone into a stream-deck style pad: pages of 12 big buttons (4×3 in landscape, 3×4 in portrait).
Each button sends a keyboard shortcut, a media key, a short key sequence or a piece of text, over USB and, if a
Bluetooth host is connected, over Bluetooth too. The host sees an ordinary keyboard: **no software to install**.

Switch to it with the side button (mode menu → DECK) or `scripts/ppctl.py mode deck`.

## The default layout

| Page | Buttons |
|---|---|
| STREAM | SCENE 1–4 (F13–F16), MIC (F17), CAMERA (F18), RECORD (F19), LIVE (F20), BRB (F21), CLIP (F22), MARKER (F23), F24 |
| MEDIA | play/pause, previous, next, mute, volume −/+, copy, paste, undo, screenshot, show desktop (Win+D), lock (Win+L) |

F13–F24 exist on the USB keyboard standard but not on real keyboards, so nothing else uses them. Bind them where
you need them, for example in **OBS**: Settings → Hotkeys → "Switch to scene" for a scene → click the field → tap
SCENE 1 on the phone. Discord (Keybinds), Streamlabs and most tools that take global hotkeys work the same way.
macOS only knows F13–F19.

Buttons with a dot are **toggles**: they light up on every other press, to remind you that the mic is muted or
the recording is running. PhoneKit can't see the app's real state, so if you toggle it on the PC, tap twice.

A key combination is **held as long as your finger is down**, so push-to-talk works. Several buttons can be held at
once; their keys go out together.

## Changing buttons

From the PC (the phone on USB):

```sh
scripts/ppctl.py deck                                     # show all pages
scripts/ppctl.py deck set 5 "MUTE" "ctrl+shift+m" red toggle   # button 5 (page 1 = 1-12, page 2 = 13-24, ...)
scripts/ppctl.py deck set 13 "SPOTIFY" "media:play" green
scripts/ppctl.py deck set 25 "SIGN" "text:Best regards," violet  # page 3 appears as soon as it has a button
scripts/ppctl.py deck page 1 "OBS"                         # rename a page
scripts/ppctl.py deck blank 12                             # empty slot
scripts/ppctl.py deck reset 5                              # back to the default (or: reset all)
```

Actions:

| Action | Example | Sent |
|---|---|---|
| combination | `ctrl+shift+f13`, `alt+tab`, `gui+d` | held while pressed |
| sequence | `ctrl+a ctrl+c` | tapped one after the other |
| media key | `media:play` `next` `prev` `stop` `mute` `volup` `voldown` `brightnessup` `brightnessdown` `calculator` `browser` `mail` | consumer-control key |
| text | `text:gg wp` | typed (US layout) |

Key names: `a`–`z`, `0`–`9`, `f1`–`f24`, `enter esc tab space backspace delete insert home end pgup pgdn up down
left right prtsc scrolllock pause capslock numlock menu`, punctuation by name (`minus equal comma period slash
semicolon quote grave lbracket rbracket backslash`). Modifiers: `ctrl shift alt gui` (also `win super cmd`),
right-hand `rctrl rshift ralt/altgr rgui`. Colours: `blue red orange amber yellow green lime teal cyan violet
pink gray white` or `#rrggbb`.

The layout is kept in the phone's settings store, so it survives reboots and updates.

## Over Bluetooth

Pair the phone once (`scripts/ppctl.py bt on`, `bt pair`, then pair "PhoneKit" on the other device); it reconnects
by itself afterwards. A host paired before PhoneKit had DECK (November 2026) has to forget PhoneKit and pair again
once to learn the F13–F24 and media keys. Text and combos follow the host's keyboard layout: set it to English (US)
for PhoneKit on the host.
