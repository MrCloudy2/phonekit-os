#!/usr/bin/env python3
"""pptest: run the hardware test suites against a connected PhoneKit phone and write a report for a PR.

  scripts/pptest.py [--device samsung-xcoverpro] [--only touch,display,...] [--skip usb,...] [--out FILE]

Suites (each is scripts/test-<name>.py; all need the phone running PhoneKit on USB):
  display, touch, power, keys, usb, input-modes, panel, phonebox (phonebox needs the phonebox agent running)

The report (default docs/verification/<device>-report.md) lists every PASS/FAIL line per suite, the
PhoneKit version, the phone's info, and how long each suite took. Attach it to a device pull request.
Suites that touch the host (usb replugs, input-modes grabbing host input devices) are run as they are; read their
headers first. This is the first step of ROADMAP M15b; stress/soak/fuzz runs come next.
"""
import argparse
import datetime
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SUITES = ["display", "touch", "power", "keys", "usb", "input-modes", "panel", "phonebox"]


def ppctl(*a, timeout=60):
    r = subprocess.run([sys.executable, os.path.join(HERE, "ppctl.py"), *map(str, a)], capture_output=True, text=True, timeout=timeout)
    return r.stdout if r.returncode == 0 else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="samsung-xcoverpro")
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    run = [s for s in SUITES if (not a.only or s in a.only.split(",")) and s not in a.skip.split(",")]
    out = a.out or os.path.join(ROOT, "docs", "verification", f"{a.device}-report.md")
    info = ppctl("--wait", 60, "info", timeout=90)
    if not info:
        sys.exit("pptest: no PhoneKit control channel (is the phone running PhoneKit and plugged in?)")
    version = next((l.split(" ", 1)[1] for l in info.splitlines() if l.startswith("version ")), "?")
    results, total_fail = [], 0
    for s in run:
        script = os.path.join(HERE, f"test-{s}.py")
        if not os.path.exists(script):
            continue
        print(f"### {s}", flush=True)
        t0 = time.time()
        try:
            r = subprocess.run([sys.executable, script], capture_output=True, text=True, timeout=1800)
            text, code = r.stdout + r.stderr, r.returncode
        except subprocess.TimeoutExpired:
            text, code = "timed out after 30 min", 124
        dt = time.time() - t0
        lines = [l.strip() for l in text.splitlines() if l.strip().startswith(("PASS", "FAIL"))]
        npass = sum(l.startswith("PASS") for l in lines)
        nfail = sum(l.startswith("FAIL") for l in lines)
        if code and not nfail:
            nfail = 1
            lines.append(f"FAIL  suite exited with code {code}: {text.strip().splitlines()[-1] if text.strip() else ''}")
        total_fail += nfail
        print(f"    {npass} pass, {nfail} fail, {dt:.0f} s", flush=True)
        results.append((s, npass, nfail, dt, lines))
        ppctl("--wait", 60, "ping", timeout=90)  # some suites re-enumerate USB: let it settle
    with open(out, "w") as f:
        f.write(f"# pptest report: {a.device}\n\n")
        f.write(f"- Date: {datetime.datetime.now().isoformat(timespec='minutes')}\n- PhoneKit: `{version}`\n")
        f.write(f"- Result: **{'ALL PASS' if not total_fail else f'{total_fail} FAILED'}**\n\n")
        f.write("| Suite | Pass | Fail | Time |\n|---|---|---|---|\n")
        for s, p, n, dt, _ in results:
            f.write(f"| {s} | {p} | {n} | {dt:.0f} s |\n")
        for s, p, n, dt, lines in results:
            f.write(f"\n## {s}\n```\n" + "\n".join(lines) + "\n```\n")
        f.write("\n## Phone info\n```\n" + info.strip() + "\n```\n")
    print(f"report: {out}")
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
