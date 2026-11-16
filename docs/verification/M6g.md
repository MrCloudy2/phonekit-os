# M6g verification: RESCUE console (Samsung Galaxy XCover Pro)

Status: 🟨 2026-11-17. Terminal, on-screen keyboard and blind typing over the USB keyboard work against a stand-in
server (this PC). SSH-over-USB and a real server are pending.

| Check | Result |
|---|---|
| RESCUE takes the shell port (iface 04) and gives it back | ✅ `rescue: console on /dev/ttyGS2`; debug shell respawns after leaving |
| No phantom monitor on the host in RESCUE (GUD left out of the gadget) | ✅ the host sees `1d6b:0104` without the GUD interface |
| Terminal output: prompt, colours (SGR 16/256/truecolour), `ls --color`, scrolling | ✅ screenshots |
| Size sync | ✅ 155×13 landscape with the keyboard; server `tput cols/lines` = 155/13 |
| Full-screen app (htop, alternate screen, REP) | ✅ after adding REP (ncurses' `rep`); quits cleanly back to the shell |
| On-screen keyboard → server | ✅ taps on e, l, s, Enter → 4 bytes; fires where the finger lifts |
| Survives the USB re-bind at boot | ✅ the console reopens on hang-up (EIO/POLLHUP), as the control channel does |
| Mode survives reboot | ✅ after fixing an older bug: settings were loaded after `hm_init`, so the input mode always came back as SCREEN |
| Blind typing for unprepared servers (**To: KBD** key) | ✅ keys go out as USB HID key presses (US layout, Shift for symbols, arrows/PgUp/Home/End/Esc as real keys, sticky Ctrl/Alt as modifiers); host received exactly `L S ENTER` (the host device was grabbed so nothing reached the desktop); a banner shows while typing blind |
