# RESCUE mode: a pocket console for headless servers

Plug the phone into a server's USB port and pick **RESCUE** in the menu (XCover key → menu). The phone
becomes a terminal on the server's serial console: no monitor, no network and no driver needed. Every Linux
kernel has the USB serial driver (`cdc_acm`).

## One-time server setup (do it while the server still works)
The server needs a login prompt on PhoneKit's console port. It's the port with USB interface number 04; on a
server with no other USB serial devices that's `/dev/ttyACM2`. A udev rule gives it a stable name and starts a
login whenever the phone is plugged in:

```
# /etc/udev/rules.d/90-phonekit-console.rules
SUBSYSTEM=="tty", ATTRS{manufacturer}=="phonepanel", ENV{ID_USB_INTERFACE_NUM}=="04", \
  SYMLINK+="phonekit-console", TAG+="systemd", ENV{SYSTEMD_WANTS}+="serial-getty@%k.service"
```

```bash
sudo udevadm control --reload
```

Plug the phone in, choose RESCUE, press **Size** (it types `stty rows R cols C` so programs know the screen size),
and log in.

## Using it
- The keyboard has Esc, Tab, Ctrl and Alt (sticky: tap, then the key), arrows, PgUp/PgDn, Home/End, Shift (one shot)
  and **Sym** for symbols. **Hide** gives the terminal the whole screen; tap the bottom edge to bring the keyboard back.
- Terminal: VT100/xterm subset with 256 colours and truecolour, scrolling regions, the alternate screen (htop, vim,
  less, mc), DEC line drawing and REP. Tested with bash, `ls --color`, htop.
- While RESCUE is on, the port is the server's console; PhoneKit's own debug shell comes back when you leave RESCUE.
  The GUD display is left out of the USB device in RESCUE, so the server doesn't get a phantom monitor.

## A server that was never set up
Tap **To: KBD**: the keys now go to the server as a USB keyboard (its own console, blind). Log in on the console,
type `systemctl start serial-getty@ttyACM2` (or whatever tty the phone's interface 04 got), tap **To: TTY** again and
press Enter: the login prompt appears on the phone. US keyboard layout.

## From another computer (SSH)
Authorise your public key once: `scripts/ppctl.py ssh authorize "$(cat ~/.ssh/id_ed25519.pub)"`. Then
`ssh root@<phone> pk-console` puts you on the server's console through the phone (over Wi-Fi, or over the USB
network the server gets in RESCUE mode). Leave with Enter `~` `.`. Keys only; no passwords.

## Not yet (ROADMAP M6g / M11b)
- A test with a real server, and with SSH over Wi-Fi from another computer.
- BIOS/GRUB need a keyboard only: there's no OS there to run a login on the serial port.
