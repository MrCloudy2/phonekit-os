#!/usr/bin/env python3
"""The hardware gate: run pptest on every PhoneKit phone plugged into this PC, each against its own baseline.

  scripts/check-all.py            (or: make check-all)
  scripts/check-all.py --only display,touch

Each phone says which device folder it is (`ppctl info` -> `device <name>`, from its hw.c). Exit code 1 when any
phone has a regression (a check its devices/<name>/pptest-baseline.txt says passed now fails). Known failures that
are already in the baseline don't block. Writes out/check-all.md: paste it into the PR ("Tested on hardware").
Phones in devices/ that weren't connected are listed as not tested.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def phones():
    """USB serials of the connected PhoneKit phones (control ACM, interface 02)."""
    out = set()
    for tty in glob.glob("/sys/class/tty/ttyACM*"):
        d = os.path.realpath(os.path.join(tty, "device"))
        try:
            if open(os.path.join(d, "bInterfaceNumber")).read().strip() != "02":
                continue
        except OSError:
            continue
        while d != "/" and not os.path.exists(os.path.join(d, "idVendor")):
            d = os.path.dirname(d)
        try:
            if open(os.path.join(d, "manufacturer")).read().strip() == "phonepanel":
                out.add(open(os.path.join(d, "serial")).read().strip())
        except OSError:
            pass
    return sorted(out)


def device_of(serial):
    env = dict(os.environ, PPCTL_SERIAL=serial)
    r = subprocess.run([sys.executable, os.path.join(HERE, "ppctl.py"), "--wait", "30", "info"],
                       capture_output=True, text=True, env=env, timeout=60)
    for l in r.stdout.splitlines():
        if l.startswith("device "):
            return l.split(" ", 1)[1].strip()
    return None


def main():
    extra = sys.argv[1:]
    known = sorted(os.path.basename(os.path.dirname(p)) for p in glob.glob(os.path.join(ROOT, "devices/*/device.mk"))
                   if "/_template/" not in p)
    serials = phones()
    if not serials:
        sys.exit("check-all: no PhoneKit phone connected")
    rows, blocked = [], False
    for i, s in enumerate(serials, 1):
        dev = device_of(s)
        tag = f"phone {i}"  # not the serial: this summary goes into a PR
        if not dev or dev not in known:
            print(f"check-all: {tag} reports device {dev!r}, which has no devices/ folder: update its PhoneKit build")
            rows.append((tag, dev or "?", "not run (unknown device)", False)); blocked = True
            continue
        print(f"\n######## {dev} ({tag})", flush=True)
        report = os.path.join(ROOT, "docs", "verification", f"{dev}-report.md")
        subprocess.run([sys.executable, os.path.join(HERE, "pptest.py"), "--device", dev, "--serial", s, *extra])
        reg = subprocess.run([sys.executable, os.path.join(HERE, "ppregress.py"), dev, report],
                             capture_output=True, text=True)
        result = next((l.split("**")[1] for l in open(report) if l.startswith("- Result:")), "?")
        ok = reg.returncode == 0
        blocked |= not ok
        rows.append((tag, dev, f"{result}, {'no regressions' if ok else 'REGRESSIONS'}", ok))
        if not ok:
            print(reg.stdout.strip())
    tested = {r[1] for r in rows}
    md = ["| Phone | Result |", "|---|---|"] + [f"| {d} | {res} |" for _, d, res, _ in rows]
    md += [f"| {d} | not tested (not connected) |" for d in known if d not in tested]
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    with open(os.path.join(ROOT, "out", "check-all.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    print("\n" + "\n".join(md))
    print("\ncheck-all: " + ("BLOCKED: fix the regressions above" if blocked else "no regressions")
          + " (summary for the PR: out/check-all.md)")
    return 1 if blocked else 0


if __name__ == "__main__":
    sys.exit(main())
