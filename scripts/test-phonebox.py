#!/usr/bin/env python3
"""PHONEBOX-mode checks against a running, unmodified phonebox desktop agent (github.com/ersinkoc/phonebox).

  scripts/test-phonebox.py

Needs: the phone in PHONEBOX mode (USB mode B) and the phonebox agent running on this PC (it finds the phone over
adb by itself). Checks: adb sees PhoneKit, the agent set up its reverse tunnel, PhoneKit's native client is
connected and receiving the live state, pages were built, a rotation re-flows them, and a tap on a media button
reaches the agent as the same POST the phonebox app sends.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
failures = []


def pp(*args, timeout=60):
    r = subprocess.run(PPCTL + list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"ppctl {' '.join(map(str, args))}: {r.stderr.strip()}")
    return r.stdout


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def stats():
    return dict(l.split(" ", 1) for l in pp("phonebox").splitlines() if " " in l)


def main():
    if "mode B" not in pp("usb"):
        pp("usb", "mode", "B"); time.sleep(4); pp("--wait", 30, "wait", timeout=45)
    print("== adb")
    devs = subprocess.run(["adb", "devices", "-l"], capture_output=True, text=True).stdout
    check("host adb lists PhoneKit as a device", "model:PhoneKit" in devs, devs.strip().splitlines()[-1] if devs.strip() else "")
    prop = subprocess.run(["adb", "shell", "getprop", "ro.phonekit"], capture_output=True, text=True).stdout.strip()
    check("adb shell works (getprop ro.phonekit = 1)", prop == "1", repr(prop))
    print("== agent")
    end = time.time() + 30
    while time.time() < end and "tcp:8765" not in subprocess.run(["adb", "reverse", "--list"], capture_output=True, text=True).stdout:
        time.sleep(1)
    check("agent set up adb reverse tcp:8765", time.time() < end, "(is the phonebox agent running?)")
    end = time.time() + 20
    while time.time() < end and stats().get("connected") != "1":
        time.sleep(1)
    s0 = stats()
    check("native client connected to the agent's live stream", s0.get("connected") == "1", str(s0))
    time.sleep(3)
    s1 = stats()
    check("live state arriving (~1/s)", int(s1["states"]) >= int(s0["states"]) + 2, f"{s0['states']} → {s1['states']}")
    info = pp("panel", '{"op":"info"}')
    pages = int(re.search(r'"pages":(\d+)', info).group(1))
    check("phonebox pages built", pages >= 1, f"{pages} pages")
    print("== rotation")
    rot0 = re.search(r"rotation (\d+)", pp("info")).group(1)
    pp("rotate", 90); time.sleep(1.5)
    check("landscape: 12x6 grid", '"cols":12' in pp("panel", '{"op":"info"}'))
    pp("rotate", rot0); time.sleep(1.5)
    print("== taps")
    p0 = int(stats()["posts"])
    # PLAY on the default Home page's media widget (x 6..11, y 3 in the phonebox grid); find it via the layout
    pp("panel", '{"op":"page","n":0}'); time.sleep(0.5)
    landscape = '"cols":12' in pp("panel", '{"op":"info"}')
    w, h, top = (2340, 1080, 90) if landscape else (1080, 2340, 90)
    cols, rows = (12, 6) if landscape else (6, 12)
    gx, gy = (8, 3) if landscape else (3, 8)  # center column of the 3 media buttons, transposed in portrait
    lx, ly = int((gx + 0.5) * w / cols), int(top + (gy + 0.5) * (h - top) / rows)
    pp("touch", "ltap", lx, ly); time.sleep(2)
    check("media button tap → POST to the agent", int(stats()["posts"]) == p0 + 1)
    print()
    print("FAILED: " + ", ".join(failures) if failures else "ALL PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
