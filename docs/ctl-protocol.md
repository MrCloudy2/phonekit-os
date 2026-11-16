# paneld control channel (USB CDC-ACM)

A development and debug channel. It is not the Mode B widget protocol (that comes in M7).

The phone presents USB id `1d6b:0104`, manufacturer `phonepanel`, serial = `androidboot.serialno`,
with three CDC-ACM serial ports (`/dev/ttyACM*` on Linux, group `uucp`/`dialout`):

| USB interface | Port | Use |
|---|---|---|
| 00 | log | read-only mirror of paneld's log (`ppctl.py logtail`); host writes are discarded |
| 02 | control | the protocol below (`ppctl.py`) |
| 04 | shell | interactive busybox shell (`picocom $(ppctl.py ports \| awk '/shell/{print $2}')`, press Enter first) |

`ppctl.py` picks ports **by interface number, never by ttyACM index**. The log port is first on purpose:
host software that blindly opens `/dev/ttyACM0` lands there harmlessly. On the dev PC this was a joystick
daemon for an unrelated USB device that polls `/dev/ttyACM0` at ~100 Hz. With the control
channel on ACM0 it injected `55 aa …` frames and stole reply bytes, which caused the first freeze and failed transfers.

## Wire format
Both sides use raw mode, 8-bit clean.

- Request: one line, `<command> [args…]\n`. Args are split on spaces; `"double quotes"` group words.
- Response: zero or more `.data <n>\n<n raw bytes>\n` blocks, then for `run` a `.exit <code>\n`,
  then always `.ok [text]\n` or `.err <message>\n`.
- `put <path> <len> [mode]` is followed by exactly `<len>` raw bytes. The file is written to `<path>.part`
  and renamed when complete.
- Empty lines are ignored. Clients resync by sending `\nping\n` and discarding input until `.ok pong …`.
- **Only lines starting with a lowercase letter and free of control bytes are commands.** Everything else
  (echoed `.ok/.err/.data` replies, NULs, modem probes like `AT`, binary frames) is dropped silently, and the
  first few are logged in hex. Why: an earlier build answered junk with `.err`. A host tty echoing phone output
  back (before the client switched it to raw), plus 5 s write timeouts per reply, froze the main loop for minutes.
- If the host stops reading, paneld flushes its output and drops replies until the next valid command,
  instead of blocking 5 s per reply.

## Robustness
- A **watchdog thread** reboots to the normal boot partition (the ROM) if the main loop doesn't get back to
  `poll()` within 15 s (longer for `run`/`runt`/`get`/`dmesg`). Before rebooting it logs the stuck phase,
  the main thread's `wchan`, `syscall` and kernel `stack`, and triggers sysrq-w (blocked tasks).
  Read it afterwards with `make logs`.
- `get` opens files with `O_NONBLOCK`, so `/proc/kmsg` or a fifo can't wedge it.

## Commands
| Command | Does |
|---|---|
| `ping` | `.ok pong <uptime ms>` |
| `info` | version, uptime, paneld start time, fb geometry, inputs, battery sysfs values |
| `ls [dir]` | `type mode size name [-> target]` per entry |
| `get <path>` | file contents (reads to EOF, so `/proc` and `/sys` work; max 64 MiB) |
| `put <path> <len> [mode]` | upload a file |
| `run <cmd> [args]` | fork/exec with no shell, stdout+stderr captured (max 4 MiB), 30 s timeout, `.exit <code>` |
| `runt <secs> <cmd> [args]` | same with a custom timeout |
| `dmesg` | kernel log buffer |
| `pattern` | redraw the test pattern |
| `idle [secs]` | get/set the auto-reboot timeout (0 = never) |
| `reboot [target]` | `.ok`, then reboot: none = normal boot (the ROM), `recovery`, `download`, `bootloader` |
| `stall <secs>` | debug: freeze the main loop (tests the watchdog) |
| `touch tap x y` / `ltap lx ly` / `swipe x0 y0 x1 y1 [steps]` / `multi k` | synthetic touches via uinput (panel px; `ltap` = logical px) |
| `rotate [0\|90\|180\|270]` | get/set UI rotation (logical ↔ panel mapping) |
| `tstats [reset]` | touches, frames, last panel/logical point, latency min/avg/max (µs) |
| `fbdump` | raw visible frame (`ppctl screenshot out.png` wraps it) |
| `fstats [reset]` | frames presented, pixels, last damage rect, blanked |
| `clock on\|off`, `blank on\|off` | 1 Hz status clock; raw display powerdown |
| `power [wake\|off\|tick\|set <key> <value>]` | power manager status/control (see docs/verification/M4.md) |
| `usb [mode A\|B \| reconnect [ms]]` | USB mode (persisted, re-enumerates) / simulated replug |
| `store [set <key> <value>]` | persistent settings (dump/set) |
| `keys [press <action> \| inject <code> <ms>]` | key bindings; run an action; press a virtual hardware key |
| `page [n]` | built-in page (0 test, 1 status) |
| `touch hold x y ms` | stationary press (e.g. corner long-press) |

Any traffic on the channel counts as activity for the idle timeout.

## Typical loop (no buttons)
```
make deploy                                 # build, dd over adb from the ROM, reboot into phonepanel
scripts/ppctl.py --wait 90 wait             # wait for the serial port
scripts/ppctl.py idle 0                     # keep it up while working
scripts/ppctl.py info
scripts/ppctl.py reboot                     # back to the ROM
make logs                                   # kernel log lines of the phonepanel boot
```
Recovery path that doesn't need a working ROM: `scripts/ppctl.py reboot download` → `heimdall flash …`.
