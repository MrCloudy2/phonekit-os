# M4b verification: standby when the PC is gone (Samsung Galaxy XCover Pro)

Status: 🟨 2026-11-16. The three modes work in PHONEBOX mode (tested by stopping/starting the phonebox agent);
PC sleep (USB suspend) and cable unplug are wired in but not yet tested on real suspend.

Settings → **WHEN PC IS GONE** (`ppctl store set standby hold|ambient|sleep`, default `hold`):
| Mode | What happens | Back |
|---|---|---|
| KEEP LAST PAGE (`hold`) | the last page stays; the status dot turns red | live again on the next state |
| AMBIENT CLOCK (`ambient`) | backlight dimmed; full-screen thin clock, date, "PC offline · phone 68%" | page + full brightness |
| SCREEN OFF (`sleep`) | screen off (FBIOBLANK, 133 mW measured in M4) | screen on as soon as the agent's data arrives again |

"Gone" = the agent's live stream has been down for 15 s, or the USB device state is `suspended` (the host
suspended the bus: PC asleep) or `not attached` (cable out).

| Check | Result |
|---|---|
| ambient: agent stopped → ambient within ~17 s; power state `dim` | ✅ screenshot |
| ambient → agent back → page, `active` | ✅ |
| sleep: agent stopped → `off`; agent back → `active` | ✅ (first build didn't wake: the 1 s UI tick doesn't run with the screen off; the check now also runs on incoming data) |
| hold: status dot red while gone | ✅ |
| PC suspend / resume, cable unplug | ⬜ needs a real suspend of the PC |
