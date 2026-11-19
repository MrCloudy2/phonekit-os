#!/usr/bin/env python3
"""Print a phone's footprint: everything the repo decides about what goes onto that phone and how.

    scripts/footprint.py <device> [paneld-binary]

CI compares it with devices/<device>/footprint.txt (`make device=<dev> footprint-check`), next to paneld-config.txt.
A change that reaches a phone shows up in that phone's file without the phone: if your diff changes another
phone's footprint, you changed that phone, and the PR says why. Sections:

  build    device.mk settings, as written (not expanded: CI and your PC must print the same)
  kernel   defconfig, config fragment lines, kernel patches (sha256)
  image    every initramfs entry (initramfs/initramfs.spec); repo files with their sha256, built artifacts and
           local prebuilt sources by name only (they are gitignored and differ per checkout)
  flash    how scripts/phone.sh writes the image and keeps the settings store
  usb      the gadget paneld builds at boot and for each USB mode (PANELD_PRINT_USB_PLAN, paneld/gadget.c)

Only repo inputs go in, so the output is the same on every checkout of the same commit.
"""
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_VARS = ["ARCH", "TARGET_PARTITION", "TARGET_PARTITION_NAME", "TARGET_PARTITION_SIZE", "AVB_FOOTER",
              "FLASH_CMD", "KERNEL_SRC", "KERNEL_DEFCONFIG", "KERNEL_FRAGMENTS", "KERNEL_DTB_GLOB",
              "KERNEL_MAKE_FLAGS", "KERNEL_KCFLAGS"]
BUILT = {"PANELD_BIN": "built: paneld (devices/<dev>/hw.c, see paneld-config.txt)",
         "VERSION_FILE": "built: version string", "BUSYBOX": "built: busybox (scripts/build-busybox.sh)",
         "WIFI_DIR": "built: Wi-Fi tools (scripts/build-wifi-tools.sh)"}


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()[:16]


def device_mk(dev):
    """Raw assignments from device.mk (continuation lines joined, comments dropped)."""
    text = open(os.path.join(ROOT, "devices", dev, "device.mk")).read()
    text = re.sub(r"\\\n\s*", " ", text)
    vals = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].rstrip()
        m = re.match(r"^\s*([A-Z_][A-Z0-9_]*)\s*(\?=|:=|=|\+=)\s*(.*)$", line)
        if m:
            k, op, v = m.groups()
            vals[k] = (vals.get(k, "") + " " + v).strip() if op == "+=" else re.sub(r"\s+", " ", v.strip())
    return vals


def section_build(dev, mk):
    out = ["[build]"]
    for k in BUILD_VARS:
        if k in mk:
            out.append(f"{k} = {mk[k]}")
    return out


def section_kernel(dev, mk):
    out = ["[kernel]"]
    if not mk.get("KERNEL_SRC"):
        return out + ["prebuilt only (no KERNEL_SRC)"]
    out.append(f"source third_party/{mk['KERNEL_SRC']}, defconfig {mk.get('KERNEL_DEFCONFIG', '?')}")
    for frag in mk.get("KERNEL_FRAGMENTS", "").split():
        p = os.path.join(ROOT, "devices", dev, frag)
        if not os.path.exists(p):
            out.append(f"fragment {frag}: missing")
            continue
        for line in open(p):
            line = line.strip()
            if line and (not line.startswith("#") or line.endswith("is not set")):
                out.append(f"fragment {frag}: {line}")
    pdir = os.path.join(ROOT, "devices", dev, "kernel-patches")
    for name in sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []:
        if name.endswith(".patch"):
            out.append(f"patch {name} {sha(os.path.join(pdir, name))}")
    return out


def describe_source(src, dev):
    m = re.match(r"^\$\{(\w+)\}(.*)$", src)
    if m:
        var, rest = m.groups()
        if var == "FW_DIR":
            return f"local, optional: devices/{dev}/prebuilt/firmware{rest} (gitignored)"
        if var in BUILT:
            return BUILT[var] + (rest and f" {rest}")
        return f"${{{var}}}{rest}"
    p = os.path.join(ROOT, src)
    return f"repo {src} {sha(p)}" if os.path.isfile(p) else f"repo {src} (missing)"


def section_image(dev):
    out = ["[image]"]
    for raw in open(os.path.join(ROOT, "initramfs", "initramfs.spec")):
        line = raw.split("#", 1)[0].strip() if not raw.lstrip().startswith("#") else ""
        if not line:
            continue
        optional = line.startswith("?")
        f = line.lstrip("?").split()
        kind, path = f[0], f[1]
        opt = " (optional)" if optional else ""
        if kind in ("file", "tree"):
            out.append(f"{kind} {path} {f[3]}{opt} <- {describe_source(f[2], dev)}")
        elif kind == "slink":
            out.append(f"slink {path} -> {f[2]}")
        elif kind == "nod":
            out.append(f"nod {path} {f[2]} {' '.join(f[5:])}")
        else:
            out.append(f"{kind} {path} {' '.join(f[2:3])}")
    return out


def section_flash(mk):
    size = mk.get("TARGET_PARTITION_SIZE", "0")
    if not size.isdigit() or int(size) == 0:
        return ["[flash]", "partition size unknown (TARGET_PARTITION_SIZE)"]
    r = subprocess.run([os.path.join(ROOT, "scripts", "phone.sh"), "plan", size, size], capture_output=True, text=True)
    return ["[flash]", f"partition {mk.get('TARGET_PARTITION_NAME', '?')} {size} B, image padded to it"] + \
        r.stdout.strip().splitlines()


def section_usb(binary):
    if not binary or not os.path.exists(binary):
        return ["[usb]", "paneld not built: make device=<dev> out/<dev>/paneld"]
    r = subprocess.run(["qemu-aarch64-static", binary], capture_output=True, text=True, timeout=60,
                       env={**os.environ, "PANELD_PRINT_USB_PLAN": "1"})
    if r.returncode:
        return ["[usb]", f"paneld exited {r.returncode}: {r.stderr.strip()[:200]}"]
    return ["[usb]"] + r.stdout.rstrip().splitlines()


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2])
        sys.exit(2)
    dev = sys.argv[1]
    binary = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "out", dev, "paneld")
    mk = device_mk(dev)
    lines = [f"# footprint of {dev}: what the repo puts on this phone and how (scripts/footprint.py; "
             f"make device={dev} footprint-update)"]
    for sec in (section_build(dev, mk), section_kernel(dev, mk), section_image(dev), section_flash(mk),
                section_usb(binary)):
        lines += sec + [""]
    print("\n".join(lines).rstrip())


if __name__ == "__main__":
    main()
