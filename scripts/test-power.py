#!/usr/bin/env python3
"""M4 power-manager checks against a running phonepanel.

  scripts/test-power.py

Screen state machine (with short timers), wake latency, charge limiter and the
thermal paths (by moving the thresholds around the current battery state), plus
power draw per state. Restores the defaults at the end.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
failures = []
DEFAULTS = dict(dim_after_s=0, off_after_s=0, bright=128, dim=12, charge_lo=40, charge_hi=60,
                temp_limit=450, temp_crit=550, cap_cpu_when_off=1, limiter=1)


def pp(*args, timeout=90):
    r = subprocess.run(PPCTL + list(map(str, args)), capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"ppctl {' '.join(map(str, args))}: {r.stderr.strip()}")
    return r.stdout


def power(*args):
    return {l.split(" ", 1)[0]: l.split(" ", 1)[1] for l in pp("power", *args).splitlines() if " " in l}


def setp(k, v):
    return power("set", k, v)


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def measure(label, secs=8):
    out = pp("runt", secs + 15, "/bin/sh", "/tmp/pm.sh", secs, label, timeout=secs + 40).strip()
    print(f"        {out}")
    return int(out.split(" = ")[1].split()[0])


def main():
    pp("idle", 0)
    pp("put", os.path.join(HERE, "power-measure.sh"), "/tmp/pm.sh", "755")
    try:
        print("== screen state machine (dim after 4 s, off after 9 s)")
        power("wake"); setp("dim_after_s", 4); setp("off_after_s", 9)
        time.sleep(5.5)
        s = power()
        check("idle -> dim", s["state"] == "dim" and s["backlight"] == "12", f"state={s['state']} backlight={s['backlight']}")
        pp("touch", "tap", 540, 2000); time.sleep(0.6)
        s = power()
        check("touch wakes from dim", s["state"] == "active" and s["backlight"] == s["bright"], f"state={s['state']} backlight={s['backlight']} (configured {s['bright']})")
        # The kernel may hold the max below cpuinfo_max_freq on its own (Exynos 9810: the hotplug governor allows
        # 2314 MHz only with 1-2 big cores online, else 1794), so note what it allowed before paneld capped.
        awake_max = pp("sh", "cat /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq").strip()
        time.sleep(10.5)
        s = power()
        check("idle -> off (blanked)", s["state"] == "off" and "blanked 1" in pp("fstats"), f"state={s['state']}")
        cpufreq = "/sys/devices/system/cpu/cpufreq/policy4"
        fmin = pp("sh", f"cat {cpufreq}/cpuinfo_min_freq").strip()
        fmax = pp("sh", f"cat {cpufreq}/cpuinfo_max_freq").strip()
        cpu4 = pp("sh", f"cat {cpufreq}/scaling_max_freq").strip()
        check("CPUs capped while off", cpu4 == fmin, f"big-cluster max = {cpu4} (cpuinfo min {fmin})")
        t0 = time.monotonic(); s = power("wake"); rt = (time.monotonic() - t0) * 1000
        check("host wake < 300 ms (unblank)", s["state"] == "active" and 0 <= int(s["last_wake_ms"]) < 300,
              f"unblank took {s['last_wake_ms']} ms (round trip {rt:.0f} ms)")
        cpu4 = pp("sh", "cat /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq").strip()
        check("CPU caps lifted on wake", cpu4 in (fmax, awake_max),
              f"big-cluster max = {cpu4} (cpuinfo max {fmax}, before screen-off {awake_max})")
        setp("dim_after_s", 0); setp("off_after_s", 0)  # stay on for the rest

        print("== charge limiter")
        s = power("tick"); cap = int(s["battery_pct"])
        setp("charge_lo", max(cap - 10, 0)); setp("charge_hi", max(cap - 5, 1)); s = power("tick"); time.sleep(4)
        cur = int(power()["battery_current_ua"])
        # charging_enabled phones hold the battery idle (USB powers the phone); slate-mode phones (hw.c charge_ctl_invert,
        # e.g. Galaxy S8) run from the battery, so it discharges. Either way nothing may flow into the battery.
        st = pp("sh", "cat /sys/class/power_supply/battery/status").strip()
        check("above window -> charging off", s["charging_allowed"] == "0" and cur < 30000 and st != "Charging",
              f"battery {cap} %, window {cap - 10}-{cap - 5}, current {cur} uA, status {st}")
        setp("charge_lo", cap + 5); setp("charge_hi", cap + 10); s = power("tick")
        # the charger takes a few seconds to ramp back up after being off (longer under load): poll, don't guess
        for _ in range(12):
            time.sleep(1)
            cur = int(power()["battery_current_ua"])
            if cur > 50000:
                break
        check("below window -> charging on", s["charging_allowed"] == "1" and cur > 50000, f"current {cur} uA")

        print("== thermal guard")
        temp = int(power("tick")["battery_temp_dC"])
        setp("temp_limit", temp - 20); s = power("tick")
        check("over temp_limit -> charging off", s["charging_allowed"] == "0", f"battery {temp / 10:.1f} C, limit {(temp - 20) / 10:.1f} C")
        setp("temp_crit", temp - 10); s = power("tick")
        check("over temp_crit -> screen forced off", s["state"] == "off" and s["forced_off"] == "1", f"state={s['state']}")
        setp("temp_limit", 450); setp("temp_crit", 550); s = power("tick"); s = power("wake")
        check("cooled -> recovers", s["state"] == "active" and s["forced_off"] == "0" and s["charging_allowed"] == "1",
              f"state={s['state']} charging_allowed={s['charging_allowed']}")

        print("== power per state (fuel gauge, slate mode)")
        # power-measure.sh uses slate mode; keep the limiter at "charging off" so its 30 s re-assert agrees with it
        # (on slate-mode phones the limiter's own switch IS batt_slate_mode)
        cap = int(power("tick")["battery_pct"]); setp("charge_lo", max(cap - 10, 0)); setp("charge_hi", max(cap - 5, 1)); power("tick")
        # Fixed, bright screen content: on OLED panels the draw depends on the picture (a dark PHONEBOX page made
        # "active" read barely above "dim" on the Galaxy S9+), so every phone measures the same test pattern.
        pp("pattern")
        setp("bright", 128); mw_on = measure("active, backlight 128")
        setp("bright", 12); mw_dim = measure("dim, backlight 12")
        setp("bright", 128); power("off"); mw_off = measure("off (blanked, CPUs capped)")
        power("wake")
        check("off draws < 1/5 of active", mw_off * 5 < mw_on, f"{mw_on} / {mw_dim} / {mw_off} mW")
    finally:
        for k, v in DEFAULTS.items():
            setp(k, v)
        power("wake"); power("tick")

    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
