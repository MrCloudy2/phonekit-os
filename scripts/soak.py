#!/usr/bin/env python3
"""Soak test (M1 24 h, M4 charge window, M15b leaks): samples the phone every N minutes and summarises.

  scripts/soak.py run [--hours 24] [--every 5] [--out FILE.csv]
  scripts/soak.py report FILE.csv

Each sample: uptime, paneld RSS / fds / threads, battery capacity / temperature / status, power state, the
PHONEBOX link. The report flags reboots (uptime going down), memory or fd growth, and the battery leaving the
charge window.
"""
import csv
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIELDS = ["t", "uptime_s", "rss_kb", "fds", "threads", "bat_pct", "bat_temp_c", "bat_status", "power", "pb_connected", "pb_states"]
PROBE = ("cat /proc/uptime; grep VmRSS /proc/1/status; ls /proc/1/fd | wc -l; ls /proc/1/task | wc -l; "
         "cat /sys/class/power_supply/battery/capacity /sys/class/power_supply/battery/temp /sys/class/power_supply/battery/status")


def pp(*a, timeout=60):
    r = subprocess.run([sys.executable, os.path.join(HERE, "ppctl.py"), "--wait", "60", *map(str, a)],
                       capture_output=True, text=True, timeout=timeout + 70)
    return r.stdout if r.returncode == 0 else ""


def sample():
    out = pp("sh", PROBE).split("\n")
    row = {k: "" for k in FIELDS}
    row["t"] = int(time.time())
    try:
        row["uptime_s"] = int(float(out[0].split()[0]))
        row["rss_kb"] = int(out[1].split()[1])
        row["fds"], row["threads"] = int(out[2]), int(out[3])
        row["bat_pct"], row["bat_temp_c"], row["bat_status"] = int(out[4]), int(out[5]) / 10, out[6].strip()
    except (IndexError, ValueError):
        pass
    pw = pp("power")
    row["power"] = next((l.split()[1] for l in pw.splitlines() if l.startswith("state ")), "")
    pb = dict(l.split(" ", 1) for l in pp("phonebox").splitlines() if " " in l)
    row["pb_connected"], row["pb_states"] = pb.get("connected", ""), pb.get("states", "")
    return row


def run(hours, every, path):
    new = not os.path.exists(path)
    end = time.time() + hours * 3600
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        if new:
            w.writeheader()
        while time.time() < end:
            w.writerow(sample())
            f.flush()
            time.sleep(every * 60)


def report(path):
    rows = list(csv.DictReader(open(path)))
    ok = [r for r in rows if r["uptime_s"]]
    if not ok:
        print("no samples")
        return 1
    hours = (int(ok[-1]["t"]) - int(ok[0]["t"])) / 3600
    reboots = sum(1 for a, b in zip(ok, ok[1:]) if int(b["uptime_s"]) < int(a["uptime_s"]))
    missed = len(rows) - len(ok)
    rss = [int(r["rss_kb"]) for r in ok]
    fds = [int(r["fds"]) for r in ok]
    bat = [int(r["bat_pct"]) for r in ok if r["bat_pct"]]
    temp = [float(r["bat_temp_c"]) for r in ok if r["bat_temp_c"]]
    print(f"{len(ok)} samples over {hours:.1f} h ({missed} missed)")
    print(f"reboots: {reboots}")
    print(f"paneld RSS: {rss[0]} -> {rss[-1]} kB (max {max(rss)})")
    print(f"paneld fds: {fds[0]} -> {fds[-1]} (max {max(fds)})")
    print(f"battery: {min(bat)}-{max(bat)} %, last {bat[-1]} %; temperature max {max(temp):.1f} C")
    leak = rss[-1] > rss[0] * 1.5 + 2048 or fds[-1] > fds[0] + 20
    print("RESULT:", "PASS" if not reboots and not leak else "CHECK (reboot or growth above)")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "report":
        sys.exit(report(a[1]))
    hours = float(a[a.index("--hours") + 1]) if "--hours" in a else 24
    every = float(a[a.index("--every") + 1]) if "--every" in a else 5
    out = a[a.index("--out") + 1] if "--out" in a else os.path.join(HERE, "..", "build", "soak.csv")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    run(hours, every, out)
