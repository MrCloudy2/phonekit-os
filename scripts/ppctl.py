#!/usr/bin/env python3
"""ppctl — talk to paneld over its USB serial control channel (stdlib only).

usage:
  ppctl.py [--port DEV] [--wait SECS] <command> [args…]

commands:
  ping | info | dmesg | help
  ls [dir]
  get <remote> [local]          (default: print to stdout)
  put <local> <remote> [mode]   (mode in octal, default: same as the local file)
  run <cmd> [args…]             (exec on the phone, prints output, exits with its code)
  runt <secs> <cmd> [args…]     (same with a timeout)
  sh <command line>             (needs /bin/sh, i.e. a bundled busybox)
  idle [secs]                   (auto-reboot timeout; 0 = never)
  pattern                       (redraw the test pattern)
  reboot [recovery|download|bootloader]
  wait                          (just wait for the port, then ping)
  logtail [secs]                (print the live log port; default: until Ctrl-C)

The port is found by manufacturer "phonepanel" + interface number
(log 00, control 02, shell 04) unless --port is given. Interactive shell: picocom on the
interface-04 tty, see `ppctl.py ports`.
"""
import glob
import os
import select
import shlex
import stat
import sys
import termios
import time


IFACE = {"log": "00", "ctl": "02", "shell": "04"}  # USB interface of each ACM port


def find_port(which="ctl"):
    """The phonepanel tty for `which`, chosen by USB interface number, never by
    ttyACM index: other host software may grab whatever is /dev/ttyACM0."""
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        dev = os.path.realpath(os.path.join(tty, "device"))
        try:
            iface = open(os.path.join(dev, "bInterfaceNumber")).read().strip()
        except OSError:
            continue
        if iface != IFACE[which]:
            continue
        # walk up to the USB device (has idVendor)
        d = dev
        while d != "/" and not os.path.exists(os.path.join(d, "idVendor")):
            d = os.path.dirname(d)
        try:
            vid = open(os.path.join(d, "idVendor")).read().strip()
            pid = open(os.path.join(d, "idProduct")).read().strip()
            mfr = open(os.path.join(d, "manufacturer")).read().strip()
        except OSError:
            continue
        if mfr == "phonepanel":  # VID:PID differs per mode (A uses the GUD ID 1d50:614d)
            return "/dev/" + os.path.basename(tty)
    return None


class Ctl:
    def __init__(self, port, timeout=60):
        # O_NONBLOCK: don't wait for carrier; switch to raw right away so the
        # host tty never echoes phone output back to the phone.
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        # raw mode (cfmakeraw)
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                      termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        attrs[1] &= ~termios.OPOST
        attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
        attrs[2] &= ~(termios.CSIZE | termios.PARENB)
        attrs[2] |= termios.CS8
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = bytearray()
        self.timeout = timeout
        self.sync()

    def sync(self, timeout=10):
        """Discard stale replies: send a ping and skip everything until its pong."""
        time.sleep(0.05)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.buf = bytearray()
        os.write(self.fd, b"\nping\n")
        deadline = time.monotonic() + timeout
        while not self._line(deadline).startswith(".ok pong"):
            pass

    def _fill(self, deadline):
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("no answer from paneld")
        r, _, _ = select.select([self.fd], [], [], left)
        if r:
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:  # O_NONBLOCK: spurious readiness, just retry
                return
            if not chunk:
                raise ConnectionError("port closed")
            self.buf += chunk

    # bytearray: appends are amortized O(1). With immutable bytes every ~4 KiB tty read
    # copied the whole buffer, so a 3 MiB reply took ~1 GB of copying, the reader fell
    # behind, and the host tty layer dropped data (transfers > 2 MiB hung).
    def _line(self, deadline):
        start = 0
        while (i := self.buf.find(b"\n", start)) < 0:
            start = len(self.buf)
            self._fill(deadline)
        line = bytes(self.buf[:i])
        del self.buf[:i + 1]
        return line.decode(errors="replace")

    def _bytes(self, n, deadline):
        while len(self.buf) < n:
            self._fill(deadline)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def call(self, line, payload=None, timeout=None):
        """Returns (ok, message, [data blocks], exit code or None)."""
        os.write(self.fd, line.encode() + b"\n")
        if payload:
            view = memoryview(payload)
            while view:
                try:
                    n = os.write(self.fd, view[:16384])
                except BlockingIOError:  # the tty's buffer is full: wait until the phone has read some
                    select.select([], [self.fd], [], 5)
                    continue
                view = view[n:]
        deadline = time.monotonic() + (timeout or self.timeout)
        blocks, code = [], None
        while True:
            l = self._line(deadline)
            if l.startswith(".data "):
                n = int(l.split()[1])
                blocks.append(self._bytes(n, deadline))
                self._bytes(1, deadline)  # trailing newline
            elif l.startswith(".exit "):
                code = int(l.split()[1])
            elif l.startswith(".ok"):
                return True, l[4:], blocks, code
            elif l.startswith(".err"):
                return False, l[5:], blocks, code
            # anything else: stray output, ignore


def wait_port(port_arg, wait):
    deadline = time.monotonic() + wait
    while True:
        port = port_arg or find_port()
        why = "phonepanel serial port not found"
        if port and os.path.exists(port):
            try:
                return Ctl(port)
            except TimeoutError:
                why = f"{port} is there but paneld does not answer"
            except (OSError, termios.error, ConnectionError) as e:
                # the gadget re-binds once at boot (RNDIS heal): the port hangs up under us; try again
                why = f"{port}: {e}"
                wait = max(wait, 5)
                deadline = max(deadline, time.monotonic() + 5)
        if time.monotonic() >= deadline:
            sys.exit(f"ppctl: {why}" + (f" (waited {wait:.0f}s)" if wait else ""))
        time.sleep(0.25)


def write_png(path, raw, w, h, stride, bpp, scale=1, bgr=False):
    """Framebuffer bytes (little-endian ARGB8888 = B,G,R,A in memory, or RGB565) → RGB PNG."""
    import struct, zlib
    rows = []
    for y in range(0, h, scale):
        line = raw[y * stride:y * stride + w * bpp // 8]
        if bpp == 32:
            b, g, r = line[0::4], line[1::4], line[2::4]
            if bgr:  # red is the low byte on this panel
                r, b = b, r
            px = bytearray(len(b) * 3)
            px[0::3], px[1::3], px[2::3] = r, g, b
        else:  # RGB565
            v = memoryview(line).cast("H")
            px = bytearray()
            for p in v:
                px += bytes(((p >> 8) & 0xF8, (p >> 3) & 0xFC, (p << 3) & 0xF8))
        if scale > 1:
            px = b"".join(px[i:i + 3] for i in range(0, len(px), 3 * scale))
        rows.append(b"\0" + bytes(px))
    ow, oh = (w + scale - 1) // scale, len(rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def q(s):
    return '"' + s + '"' if " " in s else s


def main():
    args = sys.argv[1:]
    port, wait = None, 0.0
    while args and args[0].startswith("--"):
        if args[0] == "--port":
            port = args[1]; args = args[2:]
        elif args[0] == "--wait":
            wait = float(args[1]); args = args[2:]
        else:
            sys.exit(__doc__)
    if not args:
        sys.exit(__doc__)
    cmd, rest = args[0], args[1:]
    t0 = time.monotonic()
    if cmd == "ports":
        for w in IFACE:
            print(f"{w:6} {find_port(w) or '-'}")
        return
    if cmd == "logtail":
        end = time.monotonic() + (float(rest[0]) if rest else 1e9)
        while not (p := port or find_port("log")):
            if time.monotonic() > t0 + wait:
                sys.exit("ppctl: log port not found")
            time.sleep(0.25)
        fd = os.open(p, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[3] &= ~(termios.ECHO | termios.ICANON)
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        try:
            while time.monotonic() < end:
                r, _, _ = select.select([fd], [], [], 0.5)
                if r:
                    try:
                        sys.stdout.buffer.write(os.read(fd, 65536).replace(b"\r", b""))
                        sys.stdout.flush()
                    except BlockingIOError:
                        pass
        except KeyboardInterrupt:
            pass
        return
    c = wait_port(port, wait)
    out = sys.stdout.buffer

    if cmd == "wait":
        ok, msg, _, _ = c.call("ping")
        print(f"port up after {time.monotonic() - t0:.1f}s; {msg}")
        return
    if cmd in ("ping", "info", "dmesg", "help", "pattern", "idle", "ls"):
        ok, msg, blocks, _ = c.call(" ".join([cmd] + [q(a) for a in rest]))
    elif cmd == "get":
        ok, msg, blocks, _ = c.call("get " + q(rest[0]))
        if ok and len(rest) > 1:
            open(rest[1], "wb").write(blocks[0])
            blocks = []
            msg = f"{rest[0]} → {rest[1]}"
    elif cmd == "put":
        local, remote = rest[0], rest[1]
        data = open(local, "rb").read()
        mode = rest[2] if len(rest) > 2 else "%o" % stat.S_IMODE(os.stat(local).st_mode)
        ok, msg, blocks, _ = c.call(f"put {q(remote)} {len(data)} {mode}", payload=data, timeout=120)
        msg = msg or f"{len(data)} bytes → {remote}"
    elif cmd in ("run", "runt", "sh"):
        if cmd == "sh":
            # upload the script so any quoting survives (the wire protocol has no escapes)
            script = (" ".join(rest) + "\n").encode()
            ok, msg, _, _ = c.call(f"put /tmp/.ppctl-sh {len(script)} 755", payload=script)
            if not ok:
                sys.exit(f"ppctl: upload failed: {msg}")
            line, tmo = "run /bin/sh /tmp/.ppctl-sh", 35
        else:
            line = cmd + " " + " ".join(q(a) for a in rest)
            tmo = (int(rest[0]) + 5) if cmd == "runt" else 35
        ok, msg, blocks, code = c.call(line, timeout=tmo)
        for b in blocks:
            out.write(b)
        out.flush()
        if msg:
            print(f"ppctl: {msg}", file=sys.stderr)
        sys.exit(code if code is not None else (0 if ok else 1))
    elif cmd == "screenshot":
        # screenshot <out.png> [scale]: framebuffer → PNG (fbdev ARGB8888 / RGB565 via info)
        out_png = rest[0] if rest else "screenshot.png"
        scale = int(rest[1]) if len(rest) > 1 else 1
        ok, msg, blocks, _ = c.call("info")
        geo = dict(l.split(" ", 1) for l in blocks[0].decode().splitlines() if " " in l)
        f = geo["fb"].split()  # "1080x2340 bpp 32 stride 4320 virt 1080x4680"
        w, h = map(int, f[0].split("x"))
        bpp, stride = int(f[2]), int(f[4])
        ok, msg, blocks, _ = c.call("fbdump", timeout=120)
        if not ok:
            sys.exit(f"ppctl: {msg}")
        r_off = int(geo.get("fb_rgb_offsets", "16 8 0").split()[0])  # true panel order (quirks applied)
        write_png(out_png, blocks[0], w, h, stride, bpp, scale, bgr=(r_off == 0))
        print(f"{out_png}: {w // scale}x{h // scale}")
        return
    elif cmd == "reboot":
        # the port disappears as the phone reboots; that is success, not an error
        try:
            ok, msg, blocks, _ = c.call(" ".join([cmd] + [q(a) for a in rest]))
        except (ConnectionError, TimeoutError, OSError):
            ok, msg, blocks = True, "", []
        msg = msg or "rebooting" + (f" to {rest[0]}" if rest else "")
    else:  # anything else goes to paneld as-is (it answers .err for unknown commands)
        ok, msg, blocks, _ = c.call(" ".join([cmd] + [q(a) for a in rest]))

    for b in blocks:
        out.write(b)
    out.flush()
    if msg:
        print(msg, file=sys.stderr if blocks else sys.stdout)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
