#!/usr/bin/env python3
"""ppregress: compare a pptest report with the device's baseline, so a change for one phone can't silently break
another one.

  scripts/ppregress.py <device> [report]           compare (default report: docs/verification/<device>-report.md)
  scripts/ppregress.py <device> [report] --update  make the report the new baseline (after a verified run)

The baseline (devices/<device>/pptest-baseline.txt) records, per suite and check, how many times it passed, failed
or was skipped. A regression is a check that passed in the baseline and now fails, is skipped, or is missing.
New passes and newly fixed checks are reported too, as reminders to --update. Exit code 1 on any regression.

Check names are the text before the first double space ("PASS  rotation  90: logical ..." -> "rotation"), so
repeated checks with numbers in their names are compared by count.
"""
import collections
import datetime
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse(text):
    """{(suite, check): Counter(PASS=n, FAIL=n, SKIP=n)} from a pptest report or a baseline file."""
    res = collections.defaultdict(collections.Counter)
    suite = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("## "):
            suite = line[3:].strip()
        elif "\t" in line and not line.startswith("#"):  # baseline: suite<TAB>status<TAB>count<TAB>check
            s, st, n, name = line.split("\t", 3)
            res[(s, name)][st] += int(n)
        elif suite and line[:5] in ("PASS ", "FAIL ", "SKIP "):
            name = line[4:].strip().split("  ")[0].strip()
            if name.startswith("suite exited"):
                name = "suite exited"
            res[(suite, name)][line[:4]] += 1
    return res


def baseline_path(device):
    return os.path.join(ROOT, "devices", device, "pptest-baseline.txt")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    device = args[0]
    report = args[1] if len(args) > 1 else os.path.join(ROOT, "docs", "verification", f"{device}-report.md")
    now = parse(open(report).read())
    bpath = baseline_path(device)
    if "--update" in sys.argv:
        rev = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"], capture_output=True, text=True).stdout.strip()
        with open(bpath, "w") as f:
            f.write(f"# pptest baseline for {device}: what this phone passed at {rev}, "
                    f"{datetime.date.today().isoformat()} (scripts/ppregress.py --update)\n")
            f.write("# suite\tstatus\tcount\tcheck\n")
            for (s, name), c in sorted(now.items()):
                for st in ("PASS", "FAIL", "SKIP"):
                    if c[st]:
                        f.write(f"{s}\t{st}\t{c[st]}\t{name}\n")
        print(f"baseline written: {os.path.relpath(bpath, ROOT)} ({sum(c['PASS'] for c in now.values())} passes)")
        return 0
    if not os.path.exists(bpath):
        print(f"ppregress: no baseline for {device} yet ({os.path.relpath(bpath, ROOT)}); create one with --update "
              "after a verified run")
        return 0
    base = parse(open(bpath).read())
    ran = {s for s, _ in now}  # a partial run (pptest --only) is compared on the suites it ran
    regressions, gains = [], []
    for key in sorted(k for k in set(base) | set(now) if k[0] in ran):
        b, n = base.get(key, collections.Counter()), now.get(key, collections.Counter())
        if n["PASS"] < b["PASS"]:
            regressions.append(f"{key[0]}: {key[1]}  passed {b['PASS']}x before, now {n['PASS']}x "
                               f"(fail {n['FAIL']}, skip {n['SKIP']})")
        elif n["PASS"] > b["PASS"]:
            gains.append(f"{key[0]}: {key[1]}  passes {n['PASS']}x (baseline {b['PASS']}x)")
    for g in gains:
        print(f"  NEW   {g}")
    for r in regressions:
        print(f"  REGRESSION  {r}")
    if regressions:
        print(f"\nppregress: {len(regressions)} regression(s) against {os.path.relpath(bpath, ROOT)}")
        return 1
    print(f"ppregress: no regressions against {os.path.relpath(bpath, ROOT)}"
          + (f" ({len(gains)} new passes: consider --update)" if gains else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
