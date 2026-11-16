#!/usr/bin/env python3
"""M5 USB checks: modes A/B, runtime switching, replug recovery, settings persistence.

  scripts/test-usb.py [--replugs N] [--skip-reboot]

Runs on the Linux host the phone is plugged into: inspects the host's view of the
gadget (interface classes, network interface, HID input device) as well as paneld.
"""
import glob
import os
import select
import struct
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


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}  {detail}")
    if not cond:
        failures.append(name)


def host_device():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if open(f"{d}/manufacturer").read().strip() == "phonepanel":
                return d
        except OSError:
            pass
    return None


def interfaces():
    d = host_device()
    if not d:
        return {}
    out = {}
    for i in glob.glob(f"{d}:1.*"):
        n = int(open(f"{i}/bInterfaceNumber").read(), 16)
        cls = open(f"{i}/bInterfaceClass").read().strip()
        drv = os.path.basename(os.path.realpath(f"{i}/driver")) if os.path.exists(f"{i}/driver") else "-"
        out[n] = (cls, drv)
    return out


def wait_ctl(secs=30):
    t0 = time.monotonic()
    pp("--wait", secs, "wait", timeout=secs + 15)
    return time.monotonic() - t0


def host_netif():
    d = host_device()
    for n in glob.glob(f"{d}:1.*/net/*") if d else []:
        return os.path.basename(n)
    return None


def hid_event_node():
    name = None
    for blk in open("/proc/bus/input/devices").read().split("\n\n"):
        if "phonepanel" in blk and "H: Handlers" in blk and "B: ABS=" in blk:  # the touchscreen (not mouse/keyboard)
            for l in blk.splitlines():
                if l.startswith("H: Handlers"):
                    for h in l.split("=")[1].split():
                        if h.startswith("event"):
                            name = "/dev/input/" + h
    return name


def main():
    replugs = int(sys.argv[sys.argv.index("--replugs") + 1]) if "--replugs" in sys.argv else 50
    pp("idle", 0)

    print("== mode B (smart: USB network)")
    pp("usb", "mode", "B"); wait_ctl(); time.sleep(2)
    ifs = interfaces()
    acms = [ifs.get(i, ("?", "?"))[1] for i in (0, 2, 4)]
    check("3 ACM ports at interfaces 0/2/4", all(a == "cdc_acm" for a in acms), f"{ifs}")
    check("network function bound on the host", any(drv in ("rndis_host", "cdc_ncm") for _, drv in ifs.values()), f"{ifs}")
    nif = host_netif()
    ok_ping = False
    if nif:
        for _ in range(40):  # wait for DHCP (NetworkManager) to configure it
            r = subprocess.run(["ping", "-c1", "-W1", "10.42.0.2"], capture_output=True)
            if r.returncode == 0:
                ok_ping = True
                break
            time.sleep(1)
    check("host gets DHCP and reaches the phone (ping 10.42.0.2)", ok_ping, f"host interface {nif}")

    print("== switch to mode A (display: HID touch)")
    t0 = time.monotonic(); pp("usb", "mode", "A"); wait_ctl(); dt = time.monotonic() - t0
    pp("mode", 0)  # SCREEN: touches go out as digitizer contacts
    time.sleep(1.5)
    ifs = interfaces()
    check("re-enumerated without a phone reboot; ACMs still at 0/2/4",
          all(ifs.get(i, ("?", "?"))[1] == "cdc_acm" for i in (0, 2, 4)), f"control back after {dt:.1f} s")
    check("HID interface bound (usbhid)", any(c == "03" for c, _ in ifs.values()), f"{ifs}")
    node = None
    for _ in range(20):  # the host's hid-multitouch needs a moment after enumeration
        node = hid_event_node()
        if node:
            break
        time.sleep(0.5)
    check("host sees a phonepanel touchscreen input device", node is not None, f"{node}")
    if node:
        xs, ys = [], []
        for attempt in range(3):  # first reports can race the host driver's setup
            time.sleep(1.0)
            fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
            pp("touch", "tap", 540, 1170)  # panel centre -> HID ~16383,16383
            end = time.time() + 2
            while time.time() < end:
                r, _, _ = select.select([fd], [], [], 0.3)
                if r:
                    data = os.read(fd, 24 * 64)
                    for o in range(0, len(data) - 23, 24):
                        _, _, typ, code, val = struct.unpack("llHHi", data[o:o + 24])
                        if typ == 3 and code == 0x35: xs.append(val)
                        if typ == 3 and code == 0x36: ys.append(val)
            os.close(fd)
            if xs and ys:
                break
        okc = xs and ys and abs(xs[-1] - 16383) < 200 and abs(ys[-1] - 16383) < 200
        check("phone touch arrives on the host at the right position", bool(okc),
              f"x={xs[-1] if xs else None} y={ys[-1] if ys else None} (expect ~16383), attempt {attempt + 1}")

    # replug cycles in mode A: no network interface, so the host's NetworkManager (and
    # the desktop's connected/disconnected popups) stay quiet
    print(f"== {replugs}x soft unplug/replug (mode A)")
    worst, fails = 0.0, 0
    for i in range(replugs):
        pp("usb", "reconnect", 300)
        time.sleep(0.5)
        try:
            dt = wait_ctl(20); worst = max(worst, dt)
        except Exception:
            fails += 1
    check(f"control channel recovers after every replug", fails == 0, f"{replugs - fails}/{replugs} ok, worst {worst:.1f} s")

    print("== back to mode B")
    pp("usb", "mode", "B"); wait_ctl(); time.sleep(1)
    check("mode B again", "mode B" in pp("usb"), "")

    if "--skip-reboot" not in sys.argv:
        print("== settings persist across reboot")
        stamp = str(int(time.time()))
        pp("store", "set", "test_stamp", stamp)
        pp("reboot", "recovery")  # straight back into phonepanel
        time.sleep(8)
        wait_ctl(120); pp("idle", 0)
        st = pp("store")
        check("store survives reboot", f"test_stamp={stamp}" in st, st.splitlines()[0])
        check("USB mode survives reboot", "mode B" in pp("usb"), "")

    print(f"\n{'ALL PASSED' if not failures else 'FAILED: ' + ', '.join(failures)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
