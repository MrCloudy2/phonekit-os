#!/usr/bin/env python3
"""M2 touch checks against a running phonepanel (uses synthetic touches via uinput).

  scripts/test-touch.py [--png DIR]

Checks: 10-finger multitouch tracking, coordinate mapping in all 4 rotations,
software touch->flush latency, and that strokes land on the right pixels.
Exit code 0 = all passed.
"""
import os
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
W, H = 1080, 2340  # replaced by the phone's size in main()
failures = []


def pp(*args, timeout=60):
    r = subprocess.run(PPCTL + list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"ppctl {' '.join(map(str, args))}: {r.stderr.strip()}")
    return r.stdout


def fb_size():
    """The phone's panel size from `ppctl info` ("fb 1440x2960 ..."), so the checks fit every device."""
    for line in pp("info").splitlines():
        if line.startswith("fb "):
            w, h = line.split()[1].split("x")
            return int(w), int(h)
    raise RuntimeError("ppctl info has no fb line")


def stats():
    out = {}
    for line in pp("tstats").splitlines():
        k, _, v = line.partition(" ")
        out[k] = v.split()
    return out


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def l2p(r, lx, ly):
    return {0: (lx, ly), 90: (W - 1 - ly, lx), 180: (W - 1 - lx, H - 1 - ly), 270: (ly, H - 1 - lx)}[r]


def read_png_rgb(path):
    """Minimal PNG reader for the RGB8 non-interlaced files ppctl writes (filter 0 only)."""
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        t = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = int.from_bytes(body[:4], "big"), int.from_bytes(body[4:8], "big")
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, raw, 1 + w * 3


def pixel(img, x, y):
    w, h, raw, rs = img
    o = y * rs + 1 + x * 3
    return tuple(raw[o:o + 3])


def main():
    global W, H
    outdir = sys.argv[sys.argv.index("--png") + 1] if "--png" in sys.argv else "build"
    pp("idle", 0)
    W, H = fb_size()
    if "mode B" not in pp("usb"):  # PANEL mode: paneld owns the screen (in SCREEN the PC does)
        pp("usb", "mode", "B"); time.sleep(2); pp("--wait", 30, "wait", timeout=45)
    pp("page", 0)
    print("== multitouch")
    pp("pattern"); pp("rotate", 0); pp("tstats", "reset")
    pp("touch", "multi", 10); time.sleep(1.0)
    s = stats()
    check("10 simultaneous contacts tracked", s["touches"][0] == "10", f"touches={s['touches'][0]}")
    check("frames flushed (batched, ≤1 per vsync)", int(s["frames"][0]) > 5, f"frames={s['frames'][0]}")

    print("== rotations (logical tap -> panel position)")
    for r in (0, 90, 180, 270):
        pp("rotate", r); pp("tstats", "reset")
        lw, lh = (W, H) if r in (0, 180) else (H, W)
        lx, ly = lw // 5, lh // 7  # near the logical top-left corner
        pp("touch", "ltap", lx, ly); time.sleep(0.4)
        s = stats()
        want_p = l2p(r, lx, ly)
        got_l = tuple(map(int, s["last_logical"])); got_p = tuple(map(int, s["last_panel"]))
        # the injected panel position is integer-scaled by the driver range; allow 1 px
        okp = all(abs(a - b) <= 1 for a, b in zip(got_p, want_p))
        okl = all(abs(a - b) <= 1 for a, b in zip(got_l, (lx, ly)))
        check(f"rotation {r:3}: logical {lx},{ly}", okp and okl, f"panel {got_p} want {want_p}, logical {got_l}")
    pp("rotate", 0)

    print("== stroke pixels + latency")
    pp("pattern"); pp("tstats", "reset")
    # positions as fractions of the panel (laid out on 1080x2340: swipe 100,1800 -> 980,2200), so any size works
    pp("touch", "swipe", W * 100 // 1080, H * 1800 // 2340, W * 980 // 1080, H * 2200 // 2340, 60); time.sleep(1.5)
    s = stats()
    lat = dict(zip(s["latency_us"][0::2], map(int, s["latency_us"][1::2])))
    # event timestamp -> pan done (includes the vsync wait); 2 frames at 60 Hz = 33.4 ms
    check("software latency avg < 34 ms (≤ 2 frames)", 0 <= lat["avg"] < 34000, f"min/avg/max us = {lat['min']}/{lat['avg']}/{lat['max']}")
    png = os.path.join(outdir, "test-touch.png")
    pp("screenshot", png, timeout=180)
    img = read_png_rgb(png)
    mid = pixel(img, W // 2, H * 2000 // 2340)  # midpoint of the swipe
    off = pixel(img, W * 900 // 1080, H * 1680 // 2340)  # dark area: below the grey ramp, above the stroke
    check("stroke drawn at the touched pixels", mid[2] > 200 and mid[1] > 180 and mid[0] < 120, f"rgb{mid}")
    check("untouched area unchanged", max(off) < 60, f"rgb{off}")
    print(f"  screenshot: {png}")

    print("== glove mode (Samsung tsp cmd)")
    out = pp("sh", 'T=/sys/class/sec/tsp; echo glove_mode,1 > $T/cmd; cat $T/cmd_result; '
                   'echo glove_mode,0 > $T/cmd; cat $T/cmd_result')
    check("glove_mode command accepted", "OK" in out, out.strip().replace("\n", " | "))

    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
