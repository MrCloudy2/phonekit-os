# Using PhoneKit on the phone: buttons, the mode menu and Settings

Everything here works on the phone itself, no PC needed. Button names are for the Samsung Galaxy XCover Pro;
other phones map the same actions to the buttons they have (`key.<name>.short|long` in the settings store).

## Buttons

| Button | Short press | Hold |
|---|---|---|
| **XCover key** (left side) | next mode | **open the mode menu** |
| **Top key** | next page | previous page |
| **Volume up / down** | brighter / darker | previous / next page |
| **Power** | screen on/off | 3 s: reboot (always works) |

While the screen is off, the first press of any button only wakes it.
**Safe mode:** hold Volume down while PhoneKit starts. Saved settings are ignored for that one boot (nothing is
erased), handy if a setting made the phone hard to use.

## The mode menu

Hold the XCover key. Tap a mode, or move with Volume up/down (or the top key) and pick with XCover or Power.

| Mode | What it does |
|---|---|
| PHONEBOX | the desk dashboard for the phonebox agent (USB, or Wi-Fi when the cable is out) |
| SCREEN | USB monitor + touchscreen (Linux) |
| MOUSE / KEYBOARD / COMBO | trackpad, keyboard, or both, over USB (and Bluetooth when it's on) |
| RESCUE | a server's serial console ([guide](rescue.md)) |
| DECK | hotkey pad ([guide](deck.md)) |
| WI-FI MODEM | share the phone's Wi-Fi with the PC over USB ([guide](modem.md)) |
| SETTINGS | below |
| CLOSE | back to where you were |

A short press on the XCover key steps through the same modes without opening the menu.

## Settings

Open the mode menu → **SETTINGS**. Tap an entry to change it (each tap steps to the next value); for
**BRIGHTNESS**, tap the left half for darker and the right half for brighter. Landscape shows two columns.

| Setting | Values | Default |
|---|---|---|
| **WI-FI** | opens the Wi-Fi screen (below) | off until you set a network |
| **BLUETOOTH** | opens the Bluetooth screen (below) | off |
| ROTATION | auto, 0, 90, 180, 270 | auto |
| BRIGHTNESS | tap left half darker, right half brighter | |
| DIM AFTER | 30 s, 1, 2, 5 min, never | |
| SCREEN OFF | 1, 5, 15, 30 min, never | |
| CHARGE LIMIT | 40–60 %, 50–80 %, 70–90 %, off | 40–60 % (for phones that stay plugged in) |
| USB NETWORK | RNDIS (Linux, Windows) or NCM (macOS); takes effect after a reboot | RNDIS |
| WHEN PC IS GONE | keep the last page, ambient clock, screen off | keep the last page |
| REBOOT TO ROM | starts the OS on the boot partition (e.g. LineageOS); tap twice to confirm | |
| DOWNLOAD MODE | for flashing with heimdall/Odin; tap twice | |
| POWER OFF | until the power key or a cable; tap twice | |

## Wi-Fi

Settings → **WI-FI**. The phone scans as soon as the screen opens, even when Wi-Fi is off.

1. Tap your network. Open networks join straight away; for the others a keyboard comes up.
2. Type the password. **SHIFT** for one capital, **?123** for symbols, **SHOW** to check what you typed.
3. **CONNECT**. The top card shows *Connected to …* with the phone's address, or *Joining …*.

- **Wi-Fi** switch: on/off without forgetting the network.
- **Keep off while wired** (on by default): while a computer is on the USB cable, Wi-Fi stays off; it comes on a
  couple of seconds after you unplug (joining takes a few more). A plain charger doesn't count as wired. **WI-FI
  MODEM** keeps Wi-Fi on regardless, because it needs it. Turn this off if you want Wi-Fi with the cable in too,
  e.g. for SSH over Wi-Fi while the phone is plugged into a PC.
- **FORGET** removes the saved network and turns Wi-Fi off.
- One network is saved at a time. WPA2 (and open) networks; WPA3-only networks are marked and not supported yet.
  Passwords can't contain `"` or `\`.

## Bluetooth

Settings → **BLUETOOTH**.

- **Bluetooth** switch: on/off (off by default).
- **Keep off while wired** (on by default): with a computer on the cable, the USB keyboard/mouse does the job, so
  Bluetooth stays off; it comes back when you unplug and calls the paired device back by itself.
- **PAIR A NEW DEVICE (3 MIN)**: the phone is visible as "PhoneKit"; pair it from the other device's Bluetooth
  settings. It then works as a keyboard and mouse there (KEYBOARD, MOUSE, COMBO and DECK modes).
- **CONNECT / DISCONNECT**: call the paired device, or drop the link.
- **FORGET THE PAIRED DEVICE**: remove it (pair again to use it).

## Off unless needed

PhoneKit starts nothing you haven't asked for: Wi-Fi only with a saved network, Bluetooth only when switched on,
SSH only with an authorised key, and the radios stay off while a computer is on the cable unless you change that.
At boot the phone waits until it knows whether a computer is attached before starting a radio.

## The same from a PC

```sh
scripts/ppctl.py mode menu | mode settings [wifi|bt] | mode phonebox|screen|mouse|keyboard|combo|rescue|deck|modem
scripts/ppctl.py wifi                       # status
scripts/ppctl.py wifi set "NAME" "PASSWORD"  # you type the password; it stays on the phone
scripts/ppctl.py wifi radio on|off          # Wi-Fi switch
scripts/ppctl.py wifi wired-off on|off      # keep off while wired
scripts/ppctl.py wifi off                   # forget
scripts/ppctl.py bt on|off | bt wired-off on|off | bt pair | bt connect | bt disconnect | bt forget
```
