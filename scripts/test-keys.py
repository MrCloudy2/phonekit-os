#!/usr/bin/env python3
"""Side-button checks: short/long actions, pages, brightness, mode toggle, wake-only, remap.

  scripts/test-keys.py

Presses virtual hardware keys (uinput) that take the same evdev path as the real ones. Only keys the phone
really has are tested (paneld lists them in `ppctl info` as "keys ..."); missing ones are reported as SKIP.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
failures = []
KEY = {"power": 116, "volup": 115, "voldown": 114, "xcover": 0xfc, "bixby": 0x2bf, "top": 0x2a0}


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


def skip(name, why):
    print(f"  SKIP  {name}  {why}")


def hw_keys():
    for line in pp("info").splitlines():
        if line.startswith("keys"):
            return set(line.split()[1:])
    return set()


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
    hw = hw_keys()
    print(f"   hardware keys: {' '.join(sorted(hw)) or '(none reported)'}")

    if "top" in hw:
        print("== pages (top key, PANEL)")
        pp("page", 1); p0 = pp("page")  # start on STATUS: next/previous never enter the panel's own sub-pages
        press("top"); p1 = pp("page"); check("top short -> next page", p1 != p0, f"page {p0} -> {p1}")
        press("top", 1200); p2 = pp("page"); check("top long -> previous page", p2 == p0, f"page {p1} -> {p2} (start {p0})")
    else:
        skip("top key -> pages", "no top key on this phone")
    # which page comes next depends on state (the panel page is skipped when no layout/dashboard is loaded, and on
    # it the keys step through the dashboard's sub-pages), so start on STATUS and check behaviour: next moves away
    # from the start page, previous comes back to it
    print("== pages (volume long, PANEL)")
    pp("page", 1); p0 = pp("page")  # start on STATUS (see above)
    press("voldown", 1200); p1 = pp("page"); check("vol down long -> next page", p1 != p0, f"page {p0} -> {p1}")
    press("volup", 1200); p2 = pp("page"); check("vol up long -> previous page", p2 == p0, f"page {p1} -> {p2} (start {p0})")

    print("== brightness (volume short)")
    b0 = int(power()["bright"]); press("volup"); b1 = int(power()["bright"]); press("voldown"); press("voldown")
    b2 = int(power()["bright"])
    check("volume up/down step the backlight", b1 > b0 > b2, f"{b0} -> up {b1} -> down x2 {b2}")
    press("volup")

    mk = next((k for k in ("xcover", "bixby") if k in hw), None)
    if mk:
        print(f"== modes ({mk})")
        press(mk, 1200); time.sleep(0.5)
        check(f"{mk} long -> mode menu", "menu_open 1" in pp("mode"), "")
        press(mk, 1200); time.sleep(0.5)
        check(f"{mk} long again -> menu closed", "menu_open 0" in pp("mode"), "")
    else:
        skip("mode key -> menu", "no xcover/bixby key on this phone")

    print("== power + wake-only")
    press("power"); check("power short -> screen off", power()["state"] == "off", "")
    b = power()["bright"]; press("voldown")
    s = power()
    check("first key in the dark only wakes", s["state"] == "active" and s["bright"] == b, f"state {s['state']}, bright {b} -> {s['bright']}")

    print("== remap through the store")
    pp("page", 1); p0 = pp("page")
    pp("store", "set", "key.volup.short", "page_next")
    press("volup"); p = pp("page")
    pp("store", "set", "key.volup.short", "bright_up")
    check("key.volup.short=page_next takes effect", p != p0, f"page {p0} -> {p}")
    pp("page", 0)

    pp("power", "set", "dim_after_s", 0); pp("power", "set", "off_after_s", 0)
    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
