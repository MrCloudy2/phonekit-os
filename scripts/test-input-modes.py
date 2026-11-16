#!/usr/bin/env python3
"""M6b checks: MOUSE / KEYBOARD / COMBO modes, mode cycling, menu, GUD picture restore.

  scripts/test-input-modes.py

The phonepanel mouse and keyboard devices on this PC are grabbed exclusively (EVIOCGRAB)
while testing, so no injected click or keystroke reaches the desktop.
"""
import fcntl
import os
import select
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
EVIOCGRAB = 0x40044590
W, H = 1080, 2340
failures = []


def pp(*args, timeout=60):
    r = subprocess.run(PPCTL + list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"ppctl {' '.join(map(str, args))}: {r.stderr.strip()}")
    return r.stdout.strip()


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def host_nodes():
    mouse = kbd = None
    for blk in open("/proc/bus/input/devices").read().split("\n\n"):
        if "phonepanel" not in blk:
            continue
        ev = [h for l in blk.splitlines() if l.startswith("H: Handlers") for h in l.split("=")[1].split() if h.startswith("event")]
        if not ev:
            continue
        if "B: REL=" in blk:
            mouse = "/dev/input/" + ev[0]
        elif "B: LED=" in blk:
            kbd = "/dev/input/" + ev[0]
    return mouse, kbd


class Grab:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, EVIOCGRAB, 1)

    def drain(self, secs=0.0):
        out, end = [], time.time() + secs
        while True:
            r, _, _ = select.select([self.fd], [], [], max(0, end - time.time()))
            if not r:
                if time.time() >= end:
                    return out
                continue
            d = os.read(self.fd, 24 * 256)
            for o in range(0, len(d) - 23, 24):
                _, _, t, c, v = struct.unpack("llHHi", d[o:o + 24])
                if t in (1, 2):  # EV_KEY, EV_REL
                    out.append((t, c, v))


def keys_down(evs):
    return [c for t, c, v in evs if t == 1 and v == 1]


def main():
    pp("idle", 0); pp("power", "wake")
    pp("power", "set", "dim_after_s", 0); pp("power", "set", "off_after_s", 0)
    if "mode A" not in pp("usb"):
        pp("usb", "mode", "A"); time.sleep(2); pp("--wait", 30, "wait", timeout=45)
    for _ in range(20):
        mouse, kbd = host_nodes()
        if mouse and kbd:
            break
        time.sleep(0.5)
    check("host sees phonepanel mouse + keyboard", bool(mouse and kbd), f"mouse={mouse} keyboard={kbd}")
    if not (mouse and kbd):
        sys.exit(1)
    gm, gk = Grab(mouse), Grab(kbd)

    print("== MOUSE")
    pp("mode", 1); time.sleep(0.6); gm.drain(0.2)
    pp("touch", "swipe", 300, 900, 700, 1300, 40); ev = gm.drain(1.2)
    dx = sum(v for t, c, v in ev if t == 2 and c == 0); dy = sum(v for t, c, v in ev if t == 2 and c == 1)
    check("one-finger swipe moves the pointer right/down", dx > 200 and dy > 200, f"dx={dx} dy={dy}")
    pp("touch", "tap", 540, 900); ev = gm.drain(0.8)
    check("tap = left click", 0x110 in keys_down(ev), f"{keys_down(ev)}")
    pp("touch", "mtap", 2, 900); ev = gm.drain(0.8)
    check("two-finger tap = right click", 0x111 in keys_down(ev), f"{keys_down(ev)}")
    pp("touch", "mtap", 3, 900); ev = gm.drain(0.8)
    check("three-finger tap = middle click", 0x112 in keys_down(ev), f"{keys_down(ev)}")
    pp("touch", "mdrag", 2, 700, 1200); ev = gm.drain(1.2)
    wheel = sum(v for t, c, v in ev if t == 2 and c == 8)
    check("two-finger drag scrolls", wheel != 0, f"wheel={wheel}")
    pp("touch", "hold", 200, H - 150, 400); ev = gm.drain(0.9)
    check("bottom-left zone = left button", 0x110 in keys_down(ev), f"{keys_down(ev)}")

    print("== KEYBOARD")
    pp("mode", 2); time.sleep(0.6); gk.drain(0.2)
    kb_top, row_h = H * 12 // 100, (H - H * 12 // 100) // 6
    def key_xy(row, col, units):  # centre of a key, matching hid_modes.c's layout
        total = sum(units); x0 = sum(units[:col]) * W // total; x1 = sum(units[:col + 1]) * W // total
        return (x0 + x1) // 2, kb_top + row * row_h + row_h // 2
    ten = [10] * 11
    ax, ay = key_xy(2, 0, ten)                      # A
    pp("touch", "tap", ax, ay); ev = gk.drain(0.8)
    check("tap A types KEY_A", 30 in keys_down(ev), f"{keys_down(ev)}")
    sx, sy = key_xy(3, 0, [15] + [10] * 9 + [15])   # SHF
    pp("touch", "tap", sx, sy); time.sleep(0.3); pp("touch", "tap", ax, ay); ev = gk.drain(0.8)
    check("sticky Shift then A = Shift+A", 42 in keys_down(ev) and 30 in keys_down(ev), f"{keys_down(ev)}")

    print("== COMBO")
    pp("mode", 3); time.sleep(0.6); gm.drain(0.2); gk.drain(0.2)
    pp("touch", "tap", 540, 300); ev = gm.drain(0.8)
    check("tap in the trackpad strip = click", 0x110 in keys_down(ev), f"{keys_down(ev)}")
    kb_top = H * 38 // 100; row_h = (H - kb_top) // 6
    pp("touch", "tap", key_xy(2, 0, ten)[0], kb_top + 2 * row_h + row_h // 2); ev = gk.drain(0.8)
    check("keys still type in combo", 30 in keys_down(ev), f"{keys_down(ev)}")

    print("== KEYBOARD in landscape (rotation 90)")
    pp("rotate", 90); pp("mode", 2); time.sleep(0.8); gk.drain(0.2)
    LW, LH = H, W  # logical size after rotation
    kb_top = LH * 12 // 100; row_h = (LH - kb_top) // 6
    total = sum(ten); x0 = 0; x1 = ten[0] * LW // total
    pp("touch", "ltap", (x0 + x1) // 2, kb_top + 2 * row_h + row_h // 2); ev = gk.drain(0.8)
    check("landscape: tapping A (logical coords) types KEY_A", 30 in keys_down(ev), f"{keys_down(ev)}")
    pp("rotate", 0); time.sleep(0.5)

    print("== modes + menu")
    pp("mode", 0); time.sleep(1.5)
    before = os.path.join("build", "gud-before.png"); after = os.path.join("build", "gud-after.png")
    pp("screenshot", before, 4, timeout=120)
    pp("mode", "next"); time.sleep(0.5)
    s = pp("mode"); check("mode next: SCREEN -> MOUSE", "input_mode MOUSE" in s, s.splitlines()[0])
    pp("mode", "menu"); time.sleep(0.4)
    check("menu opens", "menu_open 1" in pp("mode"), "")
    pp("keys", "inject", 114, 120); time.sleep(0.5)   # vol down: next item (MOUSE -> KEYBOARD)
    pp("keys", "inject", 0xfc, 120); time.sleep(0.6)  # xcover: select
    s = pp("mode")
    check("menu: vol down + xcover selects KEYBOARD", "input_mode KEYBOARD" in s and "menu_open 0" in s, s.replace("\n", " "))
    pp("mode", 0); time.sleep(1.5)
    pp("screenshot", after, 4, timeout=120)
    same = open(before, "rb").read() == open(after, "rb").read()
    check("SCREEN: host picture restored after other modes", same, "identical screenshots" if same else "differ")

    pp("power", "set", "dim_after_s", 0); pp("power", "set", "off_after_s", 0)
    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
