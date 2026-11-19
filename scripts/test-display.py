#!/usr/bin/env python3
"""M3 display-pipeline checks against a running phonepanel.

  scripts/test-display.py

- idle screen with the 1 Hz clock: ~1 frame/s, damage = only the clock rectangle
- idle screen, clock off: zero frames (no page flips at all)
- paneld CPU use < 1 % of one core; CPUs spend most time in the deepest idle state
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
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


def kv(text):
    return {l.split(" ", 1)[0]: l.split(" ", 1)[1].split() for l in text.splitlines() if " " in l}


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


PROBE = r'''
hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
t() { awk '{print $14+$15}' /proc/1/stat; }
idle() { cat /sys/devices/system/cpu/cpu*/cpuidle/state1/time | awk '{s+=$1} END {print s}'; }
c0=$(t); i0=$(idle); u0=$(awk '{print $1}' /proc/uptime)
sleep SECS
c1=$(t); i1=$(idle); u1=$(awk '{print $1}' /proc/uptime)
awk -v c0=$c0 -v c1=$c1 -v i0=$i0 -v i1=$i1 -v u0=$u0 -v u1=$u1 -v hz=$hz 'BEGIN {
  el = u1 - u0;
  printf "paneld_cpu_pct %.3f\n", (c1 - c0) / hz / el * 100;
  printf "deep_idle_pct %.1f\n", (i1 - i0) / 1e6 / (el * 8) * 100;   # state1 = cluster sleep, 8 cpus
}'
'''


def main():
    pp("idle", 0)
    W, H = fb_size()
    pp("page", 0)  # the test page: nothing else (e.g. a PHONEBOX dashboard on the panel page) may draw
    pp("pattern"); pp("clock", "on")
    print("== idle screen, 1 Hz clock")
    pp("fstats", "reset"); time.sleep(10.5)
    s = kv(pp("fstats"))
    frames = int(s["frames"][0]); dmg = list(map(int, s["last_damage"]))
    check("about 1 frame per second", 9 <= frames <= 12, f"frames in 10.5 s = {frames}")
    check("damage = clock rectangle only", dmg[2] * dmg[3] < W * H // 20,
          f"last damage x,y,w,h = {dmg} ({dmg[2] * dmg[3] * 100 // (W * H)} % of screen)")

    print("== idle screen, clock off")
    pp("clock", "off"); time.sleep(1.5); pp("fstats", "reset"); time.sleep(10)
    s = kv(pp("fstats"))
    check("zero frames when nothing changes", s["frames"][0] == "0", f"frames in 10 s = {s['frames'][0]}")

    print("== CPU (20 s, clock on)")
    pp("clock", "on")
    probe = PROBE.replace("SECS", "20")  # ppctl sh runs with a 30 s limit
    out = kv(pp("sh", probe, timeout=120))
    cpu = float(out["paneld_cpu_pct"][0]); deep = float(out["deep_idle_pct"][0])
    check("paneld CPU < 1 % of one core", cpu < 1.0, f"{cpu:.3f} %")
    check("CPUs mostly in deepest idle (cluster sleep)", deep > 80, f"{deep:.1f} % of CPU time")

    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
