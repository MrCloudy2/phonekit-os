#!/usr/bin/env python3
"""M7 PANEL-mode checks: PPJ/1 over TCP (protocol/PPJ1.md).

  scripts/test-panel.py [--fuzz N] [--png DIR]

Puts the phone in PANEL mode (USB network), then checks: info/ping/errors, layout + set
rendering (screenshot pixels), button/toggle/slider events from injected touches, pages,
newest-client-wins, latency, and a fuzz run (random/malformed/over-long lines) after which
paneld must still answer and must not have restarted.
"""
import json
import os
import random
import socket
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "host"))
from ppj import Panel  # noqa: E402

PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
HOST = "10.42.0.2"
failures = []


def pp(*args, timeout=90):
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


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def info_field(key):
    for line in pp("info").splitlines():
        if line.startswith(key + " "):
            return line.split(" ", 1)[1]
    return None


def read_png_rgb(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        t, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = int.from_bytes(body[:4], "big"), int.from_bytes(body[4:8], "big")
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    return w, h, zlib.decompress(idat), 1 + w * 3


def pixel(img, x, y):
    w, h, raw, rs = img
    o = y * rs + 1 + x * 3
    return tuple(raw[o:o + 3])


def wait_net(secs=60):
    end = time.time() + secs
    while time.time() < end:
        try:
            socket.create_connection((HOST, 7700), timeout=1).close()
            return True
        except OSError:
            time.sleep(1)
    return False


def touch(*a):
    pp("touch", *a)


# Default portrait grid 6x12 (on 1080x2340: cell 180 x 195) below the title bar.
def cell_center(p_info, x, y, w=1, h=1):
    W, H, cols, rows = p_info["w"], p_info["h"], p_info["cols"], p_info["rows"]
    top = 90  # title bar (panel.c)
    cw, ch = W / cols, (H - top) / rows
    return int((x + w / 2) * cw), int(top + (y + h / 2) * ch)


def main():
    outdir = sys.argv[sys.argv.index("--png") + 1] if "--png" in sys.argv else "build"
    nfuzz = int(sys.argv[sys.argv.index("--fuzz") + 1]) if "--fuzz" in sys.argv else 3000
    os.makedirs(outdir, exist_ok=True)
    pp("idle", 0)
    if "mode B" not in pp("usb"):
        pp("usb", "mode", "B"); time.sleep(3); pp("--wait", 30, "wait", timeout=45)
    pp("rotate", 0)
    pp("page", 2)  # UI_PAGE_PANEL
    print("== connect")
    check("TCP 7700 reachable over the USB network", wait_net())
    up0 = int(info_field("uptime_ms"))
    p = Panel(HOST)

    print("== basics")
    inf = p.info()
    check("info reports grid + size", inf.get("cols") == 6 and inf.get("rows") == 12 and (inf.get("w"), inf.get("h")) == fb_size(), str(inf))
    t = time.perf_counter(); r = p.request({"op": "ping"}, "pong"); rtt = (time.perf_counter() - t) * 1000
    check("ping → pong", r.get("op") == "pong", f"{rtt:.1f} ms")
    check("unknown op → error", p.request({"op": "nope"}, "error").get("msg") == "unknown op")
    check("set on unknown id → error", p.request({"op": "set", "id": "zz", "value": 1}, "error").get("op") == "error")

    print("== layout + render")
    W = [
        {"id": "g", "type": "gauge", "x": 0, "y": 0, "w": 6, "h": 2, "label": "G", "min": 0, "max": 100, "color": "#ff0000"},
        {"id": "gr", "type": "graph", "x": 0, "y": 2, "w": 6, "h": 3, "label": "GR"},
        {"id": "v", "type": "value", "x": 0, "y": 5, "w": 3, "h": 2, "label": "V", "decimals": 1},
        {"id": "c", "type": "clock", "x": 3, "y": 5, "w": 3, "h": 2},
        {"id": "tg", "type": "toggle", "x": 0, "y": 7, "w": 3, "h": 2, "text": "T"},
        {"id": "b", "type": "button", "x": 3, "y": 7, "w": 3, "h": 2, "text": "B"},
        {"id": "s", "type": "slider", "x": 0, "y": 9, "w": 6, "h": 2, "label": "S", "min": 0, "max": 100, "value": 0},
        {"id": "l", "type": "label", "x": 0, "y": 11, "w": 6, "h": 1, "text": "LABEL"},
    ]
    r = p.layout(W, title="TEST")
    check("layout accepted (8 widgets)", r.get("op") == "ok" and r.get("widgets") == 8, str(r))
    p.set("g", value=100)
    for i in range(60):
        p.set("gr", push=i % 10)
    p.set("v", value=12.3)
    time.sleep(0.4)
    png = os.path.join(outdir, "panel.png")
    pp("screenshot", png, 1)
    img = read_png_rgb(png)
    gx, gy = cell_center(inf, 2, 0, 2, 2)
    px = pixel(img, gx, gy)
    check("gauge at 100% filled with its #ff0000 accent", px[0] > 200 and px[1] < 60 and px[2] < 60, f"rgb{px}")
    p.set("g", value=0)
    time.sleep(0.3)
    pp("screenshot", png, 1)
    px2 = pixel(read_png_rgb(png), gx, gy)
    check("gauge redraws empty after set 0", px2[0] < 120, f"rgb{px2}")

    print("== events")
    bx, by = cell_center(inf, 3, 7, 3, 2)
    n, lat = 0, []
    for _ in range(10):
        t = time.perf_counter()
        touch("tap", bx, by)
        evs = [e for e in p.events(timeout=0.5) if e.get("id") == "b"]
        if evs:
            n += 1
            lat.append((time.perf_counter() - t) * 1000)
    check("10 button taps → 10 press events", n == 10, f"{n}/10")
    if lat:
        print(f"        tap → event (incl. ppctl + uinput): median {sorted(lat)[len(lat) // 2]:.0f} ms")
    tx, ty = cell_center(inf, 0, 7, 3, 2)
    vals = []
    for _ in range(4):
        touch("tap", tx, ty)
        vals += [e["value"] for e in p.events(timeout=0.5) if e.get("id") == "tg"]
    check("toggle flips 1,0,1,0", vals == [1, 0, 1, 0], str(vals))
    sx, sy = cell_center(inf, 0, 9, 6, 2)
    ends = []
    pw = inf["w"]
    for x0, x1 in [(pw * 9 // 100, pw * 93 // 100), (pw * 93 // 100, pw * 6 // 100)]:  # 100->1000, 1000->60 on 1080
        touch("swipe", x0, sy, x1, sy, 40)
        ev = [e for e in p.events(timeout=1.0) if e.get("id") == "s"]
        ends.append((len(ev), ev[-1]["value"] if ev else None))
    check("slider drag → several changes, ends near max / min",
          all(c >= 3 for c, _ in ends) and ends[0][1] > 90 and ends[1][1] < 10, str(ends))

    print("== pages")
    r = p.layout([{"id": "p1", "type": "label", "x": 0, "y": 0, "w": 6, "h": 1, "text": "PAGE 2"}], page=1, title="P2")
    check("layout on page 1", r.get("op") == "ok")
    check("page 1 shown", p.page(1).get("op") == "ok" and p.info().get("page") == 1)
    check("info counts 2 pages", p.info().get("pages") == 2)
    p.page(0)
    r = p.request({"op": "layout", "page": 9, "widgets": []}, "ok")
    check("page out of range → error", r.get("op") == "error", str(r))
    many = [{"id": f"w{i}", "type": "label", "x": i % 6, "y": i // 6 % 12, "w": 1, "h": 1, "text": "X"} for i in range(60)]
    r = p.request({"op": "layout", "page": 2, "widgets": many}, "ok")
    check("60 widgets → capped (≤48) or refused, never crashes",
          (r.get("op") == "ok" and r.get("widgets", 99) <= 48) or r.get("op") == "error", str(r)[:80])
    p.request({"op": "clear", "page": 2}, "ok")

    print("== newest client wins")
    p2 = Panel(HOST)
    check("second client answers", p2.info().get("op") == "info")
    try:
        p.sock.settimeout(1)
        dead = p.sock.recv(1) == b""
    except OSError:
        dead = True
    check("first client was disconnected", dead)
    p = p2

    print(f"== fuzz ({nfuzz} lines)")
    rnd = random.Random(7)
    seeds = [json.dumps(w) for w in W] + ['{"op":"layout","widgets":[', '{"op":"set","id":"g","value":1e309}',
                                          '{"op":"set","id":"' + "A" * 300 + '"}', "[" * 2000, '"\\u', "{" * 500]
    kinds = 0
    for i in range(nfuzz):
        k = rnd.randrange(6)
        if k == 0:
            line = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 200))).replace(b"\n", b" ")
        elif k == 1:
            s = rnd.choice(seeds)
            cut = rnd.randrange(len(s) + 1)
            line = (s[:cut] + s[cut + 1:]).encode()
        elif k == 2:
            line = json.dumps({"op": rnd.choice(["layout", "set", "page", "clear", "info", "x"]),
                               "page": rnd.choice([-1, 0, 7, 8, 2**31, "a", None]),
                               "id": rnd.choice(["g", "", None, 5, "s" * 40]),
                               "value": rnd.choice([0, -1e300, "x", [], {}, None, 1e309 if False else 3.5]),
                               "push": rnd.choice([1, "x", None]),
                               "widgets": [{"id": str(j), "type": rnd.choice(["label", "gauge", "graph", "slider", "q"]),
                                            "x": rnd.randrange(-5, 20), "y": rnd.randrange(-5, 20),
                                            "w": rnd.randrange(-2, 20), "h": rnd.randrange(-2, 20),
                                            "min": rnd.choice([0, 5, 100]), "max": rnd.choice([0, 5, 100]),
                                            "text": "T" * rnd.randrange(0, 200), "color": rnd.choice(["#ff00ff", "#zz", "", 5])}
                                           for j in range(rnd.randrange(0, 60))]}).encode()
        elif k == 3:
            line = ("[" * rnd.randrange(1, 3000)).encode()
        elif k == 4:
            line = b"x" * rnd.randrange(16000, 40000)  # over-long
        else:
            line = rnd.choice(seeds).encode()
        kinds |= 1 << k
        p.sock.sendall(line + b"\n")
        if i == 0:
            p.sock.settimeout(10)
        if i % 200 == 0:
            # drain replies so the phone never blocks on a full socket. Raw bytes only: a reply split across reads
            # isn't valid JSON on its own (parsing here crashed the suite on the S8, 2026-11-19)
            try:
                while p.sock.recv(65536, socket.MSG_DONTWAIT):
                    pass
            except (BlockingIOError, TimeoutError):
                pass
            p.buf = b""
    time.sleep(1)
    p = Panel(HOST)
    check("after fuzz: still answers ping", p.request({"op": "ping"}, "pong").get("op") == "pong")
    check("after fuzz: valid layout still works", p.layout(W[:2]).get("op") == "ok")
    up1 = int(info_field("uptime_ms"))
    check("after fuzz: paneld did not restart", up1 > up0, f"uptime {up0} → {up1} ms")
    p.request({"op": "clear"}, "ok")

    print()
    print("FAILED: " + ", ".join(failures) if failures else "ALL PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
