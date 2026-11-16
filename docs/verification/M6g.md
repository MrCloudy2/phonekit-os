# M6g verification: RESCUE console (Samsung Galaxy XCover Pro)

Status: 🟨 2026-11-16. Terminal and on-screen keyboard work against a stand-in server (this PC: `socat` connects
`/dev/ttyACM2` to `bash`, as `serial-getty` would). Bootstrap macro, SSH-over-USB and a real server are pending.

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
