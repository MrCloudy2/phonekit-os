#!/usr/bin/env python3
"""Side-button checks: short/long actions, pages, brightness, mode toggle, wake-only, remap.

  scripts/test-keys.py

Presses virtual hardware keys (uinput) that take the same evdev path as the real ones.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
failures = []
KEY = {"power": 116, "volup": 115, "voldown": 114, "xcover": 0xfc, "top": 0x2a0}


def pp(*args, timeout=60):
    r = subprocess.run(PPCTL + list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"ppctl {' '.join(map(str, args))}: {r.stderr.strip()}")
    return r.stdout.strip()


def press(name, ms=120):
    pp("keys", "inject", KEY[name], ms)
    time.sleep(ms / 1000 + 0.5)


def power():
    return {l.split(" ", 1)[0]: l.split(" ", 1)[1] for l in pp("power").splitlines() if " " in l}


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def main():
    pp("idle", 0); pp("power", "wake")
    if "mode B" not in pp("usb"):  # PANEL: paneld owns the screen (pages)
        pp("usb", "mode", "B"); time.sleep(2); pp("--wait", 30, "wait", timeout=45)
    pp("page", 0)
    pp("power", "set", "dim_after_s", 0); pp("power", "set", "off_after_s", 0)
    print("== default bindings")
    print("   " + pp("keys").replace("\n", "\n   "))

    print("== pages (top key, PANEL)")
    press("top"); check("top short -> next page", pp("page") == "1", f"page {pp('page')}")
    press("top", 1200); check("top long -> previous page", pp("page") == "0", f"page {pp('page')}")

    print("== brightness (volume short)")
    b0 = int(power()["bright"]); press("volup"); b1 = int(power()["bright"]); press("voldown"); press("voldown")
    b2 = int(power()["bright"])
    check("volume up/down step the backlight", b1 > b0 > b2, f"{b0} -> up {b1} -> down x2 {b2}")
    press("volup")

    print("== modes (xcover)")
    press("xcover", 1200); time.sleep(0.5)
    check("xcover long -> mode menu", "menu_open 1" in pp("mode"), "")
    press("xcover", 1200); time.sleep(0.5)
    check("xcover long again -> menu closed", "menu_open 0" in pp("mode"), "")

    print("== power + wake-only")
    press("power"); check("power short -> screen off", power()["state"] == "off", "")
    b = power()["bright"]; press("voldown")
    s = power()
    check("first key in the dark only wakes", s["state"] == "active" and s["bright"] == b, f"state {s['state']}, bright {b} -> {s['bright']}")

    print("== remap through the store")
    pp("store", "set", "key.volup.short", "page_next")
    press("volup"); p = pp("page")
    pp("store", "set", "key.volup.short", "bright_up")
    check("key.volup.short=page_next takes effect", p == "1", f"page {p}")
    pp("page", 0)

    pp("power", "set", "dim_after_s", 0); pp("power", "set", "off_after_s", 0)
    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
