#!/usr/bin/env python3
"""Demo PANEL dashboard: this PC's CPU, RAM, network and load on the phone (Linux host).

  host/demo_dashboard.py [--seconds N]

Buttons: PAGE 2 shows a second page; the MUTE toggle and VOL slider only print their events here.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppj import Panel  # noqa: E402


def cpu_times():
    f = open("/proc/stat").readline().split()[1:]
    v = list(map(int, f))
    return sum(v), v[3] + v[4]


def mem_pct():
    m = {l.split(":")[0]: int(l.split()[1]) for l in open("/proc/meminfo")}
    return 100.0 * (1 - m["MemAvailable"] / m["MemTotal"])


def net_bytes():
    tot = 0
    for l in open("/proc/net/dev").readlines()[2:]:
        name, data = l.split(":", 1)
        if name.strip() != "lo":
            f = data.split()
            tot += int(f[0]) + int(f[8])
    return tot


def main():
    secs = float(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 1e9
    p = Panel()
    print("phone:", p.info())
    p.layout(page=0, title="DESK", widgets=[
        {"id": "clock", "type": "clock", "x": 0, "y": 0, "w": 6, "h": 2},
        {"id": "cpu", "type": "gauge", "x": 0, "y": 2, "w": 6, "h": 2, "label": "CPU", "unit": "%", "min": 0, "max": 100},
        {"id": "ram", "type": "gauge", "x": 0, "y": 4, "w": 6, "h": 2, "label": "RAM", "unit": "%", "min": 0, "max": 100, "color": "#e09040"},
        {"id": "net", "type": "graph", "x": 0, "y": 6, "w": 6, "h": 3, "label": "NETWORK MB/S", "decimals": 1, "color": "#50d090"},
        {"id": "mute", "type": "toggle", "x": 0, "y": 9, "w": 3, "h": 3, "text": "MUTE"},
        {"id": "p2", "type": "button", "x": 3, "y": 9, "w": 3, "h": 3, "text": "PAGE 2"},
    ])
    p.layout(page=1, title="MORE", widgets=[
        {"id": "load", "type": "value", "x": 0, "y": 0, "w": 6, "h": 3, "label": "LOAD 1 MIN", "decimals": 2},
        {"id": "up", "type": "value", "x": 0, "y": 3, "w": 6, "h": 3, "label": "UPTIME", "unit": "H", "decimals": 1},
        {"id": "vol", "type": "slider", "x": 0, "y": 6, "w": 6, "h": 3, "label": "VOLUME", "min": 0, "max": 100, "value": 50},
        {"id": "back", "type": "button", "x": 0, "y": 9, "w": 6, "h": 3, "text": "BACK"},
    ])
    p.page(0)
    t0, (c0, i0), n0, end = time.monotonic(), cpu_times(), net_bytes(), time.monotonic() + secs
    while time.monotonic() < end:
        time.sleep(1.0)
        (c1, i1), n1, now = cpu_times(), net_bytes(), time.monotonic()
        p.set("cpu", value=round(100.0 * (1 - (i1 - i0) / max(1, c1 - c0)), 1))
        p.set("ram", value=round(mem_pct(), 1))
        p.set("net", push=round((n1 - n0) / (now - t0) / 1e6, 2))
        p.set("load", value=float(open("/proc/loadavg").read().split()[0]))
        p.set("up", value=float(open("/proc/uptime").read().split()[0]) / 3600)
        c0, i0, n0, t0 = c1, i1, n1, now
        for ev in p.events(0):
            print("event:", ev)
            if ev["id"] == "p2":
                p.page(1)
            elif ev["id"] == "back":
                p.page(0)


if __name__ == "__main__":
    main()
