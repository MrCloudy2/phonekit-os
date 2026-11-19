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


def mute(on):
    """Test mode on the phone under test: it ignores the phonebox agent's page switches and wake/sleep requests
    (which would break the display/keys/power suites), while the agent keeps serving any other phones."""
    ppctl("phonebox", "mute", 1 if on else 0)


def start_state():
    """Every suite starts from the same state: USB mode B (PHONEBOX), where the phone draws its own screen. In
    mode A the host's GUD stream (SCREEN) or the DECK keys draw over the test page, so whatever the previous
    suite or the last boot left behind decided whether display/touch passed. Re-enumerates only when the phone
    isn't in mode B already."""
    if "mode B" in ppctl("usb"):
        return
    ppctl("mode", "phonebox")
    time.sleep(2)
    ppctl("--wait", 60, "ping", timeout=90)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="samsung-xcoverpro")
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--serial", default=os.environ.get("PPCTL_SERIAL", ""),
                    help="USB serial of the phone to test, when several PhoneKit phones are connected")
    a = ap.parse_args()
    if a.serial:  # every suite talks to this phone only: ppctl through PPCTL_SERIAL, adb through ANDROID_SERIAL
        os.environ["PPCTL_SERIAL"] = os.environ["ANDROID_SERIAL"] = a.serial
    run = [s for s in SUITES if (not a.only or s in a.only.split(",")) and s not in a.skip.split(",")]
    out = a.out or os.path.join(ROOT, "docs", "verification", f"{a.device}-report.md")
    info = ppctl("--wait", 60, "info", timeout=90)
    if not info:
        sys.exit("pptest: no PhoneKit control channel (is the phone running PhoneKit and plugged in?)")
    version = next((l.split(" ", 1)[1] for l in info.splitlines() if l.startswith("version ")), "?")
    results, total_fail = [], 0
    try:
        total_fail = run_suites(run, results)
    finally:
        mute(False)
    write_report(out, a.device, version, info, results, total_fail)
    # a change for one phone must not break another: compare with what this phone passed before
    print("== regressions against devices/%s/pptest-baseline.txt" % a.device, flush=True)
    reg = subprocess.run([sys.executable, os.path.join(HERE, "ppregress.py"), a.device, out]).returncode
    return 1 if total_fail or reg else 0


def run_suites(run, results):
    total_fail = 0
    for s in run:
        script = os.path.join(HERE, f"test-{s}.py")
        if not os.path.exists(script):
            continue
        print(f"### {s}", flush=True)
        start_state()
        mute(s != "phonebox")
        t0 = time.time()
        try:
            r = subprocess.run([sys.executable, script], capture_output=True, text=True, timeout=1800)
            text, code = r.stdout + r.stderr, r.returncode
        except subprocess.TimeoutExpired:
            text, code = "timed out after 30 min", 124
        dt = time.time() - t0
        # result lines are "PASS  name", "FAIL  name", "SKIP  name"; a suite's closing "FAILED: a, b" summary is not one
        lines = [l.strip() for l in text.splitlines() if l.strip().startswith(("PASS ", "FAIL ", "SKIP "))]
        npass = sum(l.startswith("PASS ") for l in lines)
        nfail = sum(l.startswith("FAIL ") for l in lines)
        if code and not nfail:
            nfail = 1
            lines.append(f"FAIL  suite exited with code {code}: {text.strip().splitlines()[-1] if text.strip() else ''}")
        total_fail += nfail
        nskip = sum(l.startswith("SKIP ") for l in lines)
        print(f"    {npass} pass, {nfail} fail{f', {nskip} skipped (not on this phone)' if nskip else ''}, {dt:.0f} s", flush=True)
        results.append((s, npass, nfail, dt, lines))
        ppctl("--wait", 60, "ping", timeout=90)  # some suites re-enumerate USB: let it settle
    return total_fail


def write_report(out, device, version, info, results, total_fail):
    with open(out, "w") as f:
        f.write(f"# pptest report: {device}\n\n")
        f.write(f"- Date: {datetime.datetime.now().isoformat(timespec='minutes')}\n- PhoneKit: `{version}`\n")
        f.write(f"- Result: **{'ALL PASS' if not total_fail else f'{total_fail} FAILED'}**\n\n")
        f.write("| Suite | Pass | Fail | Time |\n|---|---|---|---|\n")
        for s, p, n, dt, _ in results:
            f.write(f"| {s} | {p} | {n} | {dt:.0f} s |\n")
        for s, p, n, dt, lines in results:
            f.write(f"\n## {s}\n```\n" + "\n".join(lines) + "\n```\n")
        f.write("\n## Phone info\n```\n" + info.strip() + "\n```\n")
    print(f"report: {out}")


if __name__ == "__main__":
    sys.exit(main())
