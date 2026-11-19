#!/usr/bin/env python3
"""Estimate the real battery capacity by coulomb counting while the battery discharges (or charges).

  scripts/battery-capacity.py [--minutes 360] [--interval 30] [--csv FILE] [--wait-for Discharging|Charging]

Samples the phone's battery every --interval seconds over the control channel (capacity %, current, voltage,
and the fuel gauge's charge counter when it has one), integrates the current, and prints
  capacity_mAh ~= charge moved / percent moved * 100
Run it while the charge limiter lets the phone run from the battery (between charge_hi and charge_lo), or while it
charges. The fuel gauge's own learned capacity can be far off on an old cell; this measures what actually flows.
Stops after --minutes, when the percentage has moved by 15 points, or when the battery status flips between Charging
and Discharging (the charge limiter switched): one run measures one stretch. The current is integrated with its
sign, because the net current swings both ways while the phone's own draw competes with the charger.
"""
import argparse
import csv
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PPCTL = [sys.executable, os.path.join(HERE, "ppctl.py")]
B = "/sys/class/power_supply/battery"


def sample():
    out = subprocess.run(PPCTL + ["sh", f"cd {B}; for f in capacity batt_current_ua_now voltage_now charge_counter "
                                        "charge_now status; do echo \"$f $(cat $f 2>/dev/null)\"; done"],
                         capture_output=True, text=True, timeout=60).stdout
    d = dict(l.split(" ", 1) for l in out.splitlines() if " " in l)
    num = lambda k: int(d[k]) if d.get(k, "").lstrip("-").isdigit() else None
    return {"t": time.time(), "pct": num("capacity"), "ua": num("batt_current_ua_now"), "uv": num("voltage_now"),
            "counter_uah": num("charge_counter"), "now_uah": num("charge_now"), "status": d.get("status", "")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=360)
    ap.add_argument("--interval", type=float, default=30)
    ap.add_argument("--csv", default="")
    ap.add_argument("--wait-for", default="", help="start measuring only once the battery status is this")
    ap.add_argument("--serial", default="", help="USB serial of the phone, when several PhoneKit phones are connected")
    a = ap.parse_args()
    if a.serial:
        os.environ["PPCTL_SERIAL"] = a.serial
    if a.wait_for:
        print(f"waiting for status {a.wait_for} ...", flush=True)
        while True:
            try:
                if sample()["status"] == a.wait_for:
                    break
            except Exception:
                pass
            time.sleep(a.interval)
    rows, moved_uah = [], 0.0
    w = csv.writer(open(a.csv, "w", newline="")) if a.csv else None
    if w:
        w.writerow(["t", "pct", "ua", "uv", "counter_uah", "now_uah", "status"])
    end = time.time() + a.minutes * 60
    while time.time() < end:
        try:
            s = sample()
        except Exception as e:  # phone rebooting, USB re-enumerating: try again next interval
            print(f"sample failed: {e}", flush=True)
            time.sleep(a.interval)
            continue
        if s["pct"] is None or s["ua"] is None:
            time.sleep(a.interval)
            continue
        flip = {"Charging", "Discharging"}
        if rows and {s["status"], rows[0]["status"]} == flip:
            print(f"{time.strftime('%H:%M:%S')} status changed ({rows[0]['status']} -> {s['status']}): stopping", flush=True)
            break
        if rows:
            dt_h = (s["t"] - rows[-1]["t"]) / 3600
            moved_uah += (s["ua"] + rows[-1]["ua"]) / 2 * dt_h  # trapezoid, signed
        rows.append(s)
        if w:
            w.writerow([round(s["t"]), s["pct"], s["ua"], s["uv"], s["counter_uah"], s["now_uah"], s["status"]])
        dp = abs(s["pct"] - rows[0]["pct"])
        est = f"~{abs(moved_uah) / dp * 100 / 1000:.0f} mAh" if dp else "-"
        print(f"{time.strftime('%H:%M:%S')} {s['pct']}% {s['ua'] / 1000:+.0f} mA {s['status']:<12} "
              f"moved {moved_uah / 1000:+.0f} mAh over {dp} %: capacity {est}", flush=True)
        if dp >= 15:
            break
        time.sleep(a.interval)
    if len(rows) > 1:
        dp = abs(rows[-1]["pct"] - rows[0]["pct"])
        print(f"\nresult: {moved_uah / 1000:+.0f} mAh over {dp} % ({rows[0]['pct']} -> {rows[-1]['pct']} %, "
              f"{(rows[-1]['t'] - rows[0]['t']) / 3600:.1f} h)" + (f": capacity ~{abs(moved_uah) / dp * 100 / 1000:.0f} mAh" if dp else ""))
        c0, c1 = rows[0]["counter_uah"], rows[-1]["counter_uah"]
        if c0 is not None and c1 is not None and dp:
            print(f"fuel gauge charge_counter moved {abs(c1 - c0) / 1000:.0f} mAh: capacity ~{abs(c1 - c0) / dp * 100 / 1000:.0f} mAh")


if __name__ == "__main__":
    main()
