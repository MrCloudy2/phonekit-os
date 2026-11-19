#!/usr/bin/env python3
"""Unpack/pack Samsung's legacy boot image layout (AOSP header v0 + a separate DTB section).

Older Samsung Exynos phones (Galaxy S7/S8/S9/Note 8 era, Android 7-9) put the size of a DTB table in the
header word where AOSP later put `header_version` (offset 40), and append the DTB after the second stage.
AOSP's unpack_bootimg reads that word as a version number and fails, so import-prebuilt.sh hands these images
to this tool instead.

  samsung-bootimg.py detect <img>                  exit 0 if the image uses this layout
  samsung-bootimg.py unpack <img> <outdir>         writes kernel, ramdisk, [second], dt and prints pack args
  samsung-bootimg.py pack --kernel K --ramdisk R --dt D [--second S] --pagesize N --kernel_addr A
                     --ramdisk_addr A --second_addr A --tags_addr A --os_version_raw V [--board B]
                     [--cmdline C] -o <out.img>

Layout: header page | kernel | ramdisk | second | dt, each padded to the page size. The header id is
SHA-1 over each section followed by its size (little-endian u32), dt included, like Samsung's mkbootimg.
"""
import argparse
import hashlib
import os
import shlex
import struct
import sys

MAGIC = b"ANDROID!"
# magic, kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags_addr,
# page_size, dt_size, os_version, name[16], cmdline[512], id[32], extra_cmdline[1024]
HDR = struct.Struct("<8s10I16s512s32s1024s")


def pad(n, page):
    return (page - n % page) % page


def read_header(data):
    if len(data) < HDR.size or data[:8] != MAGIC:
        return None
    f = HDR.unpack_from(data)
    h = dict(zip(["magic", "kernel_size", "kernel_addr", "ramdisk_size", "ramdisk_addr", "second_size",
                  "second_addr", "tags_addr", "page_size", "dt_size", "os_version", "name", "cmdline", "id",
                  "extra_cmdline"], f))
    return h


def is_samsung_dt(data):
    h = read_header(data)
    if not h or h["page_size"] not in (2048, 4096, 8192, 16384):
        return False
    # AOSP header versions are 0..4; Samsung stores the DTB size here. Require the sections to add up.
    if h["dt_size"] <= 4:
        return False
    p = h["page_size"]
    end = p
    for k in ("kernel_size", "ramdisk_size", "second_size", "dt_size"):
        end += h[k] + pad(h[k], p)
    return end <= len(data)


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def cmd_unpack(img, outdir):
    data = open(img, "rb").read()
    if not is_samsung_dt(data):
        sys.exit(f"{img}: not a Samsung header-v0 + DT image")
    h = read_header(data)
    p = h["page_size"]
    os.makedirs(outdir, exist_ok=True)
    off = p
    files = {}
    for name in ("kernel", "ramdisk", "second", "dt"):
        n = h[name + "_size"]
        if n:
            with open(os.path.join(outdir, name), "wb") as f:
                f.write(data[off:off + n])
            files[name] = os.path.join(outdir, name)
        off += n + pad(n, p)
    cmdline = cstr(h["cmdline"]) + cstr(h["extra_cmdline"])
    args = ["--pagesize", hex(p), "--kernel_addr", hex(h["kernel_addr"]), "--ramdisk_addr",
            hex(h["ramdisk_addr"]), "--second_addr", hex(h["second_addr"]), "--tags_addr", hex(h["tags_addr"]),
            "--os_version_raw", hex(h["os_version"]), "--board", cstr(h["name"]), "--cmdline", cmdline]
    for name, path in files.items():
        args += ["--" + name, path]
    print(" ".join(shlex.quote(a) for a in args))
    if off < len(data):
        print(f"note: {len(data) - off} trailing bytes after the DT section (not imported)", file=sys.stderr)


def cmd_pack(a):
    p = a.pagesize
    secs = []
    for path in (a.kernel, a.ramdisk, a.second, a.dt):
        secs.append(open(path, "rb").read() if path else b"")
    sha = hashlib.sha1()
    for s in secs:
        sha.update(s)
        sha.update(struct.pack("<I", len(s)))
    cmdline = a.cmdline.encode()
    if len(cmdline) > 512 + 1023:
        sys.exit("cmdline too long")
    hdr = HDR.pack(MAGIC, len(secs[0]), a.kernel_addr, len(secs[1]), a.ramdisk_addr, len(secs[2]),
                   a.second_addr, a.tags_addr, p, len(secs[3]), a.os_version_raw, a.board.encode()[:15],
                   cmdline[:511], sha.digest().ljust(32, b"\0"), cmdline[511:])
    # mkbootimg splits the cmdline at 511 bytes (512 with the terminator); keep the same convention
    with open(a.o, "wb") as f:
        f.write(hdr + b"\0" * pad(len(hdr), p))
        for s in secs:
            f.write(s + b"\0" * pad(len(s), p))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "detect" and len(sys.argv) == 3:
        with open(sys.argv[2], "rb") as f:
            sys.exit(0 if is_samsung_dt(f.read(HDR.size + 64 * 1024 * 1024)) else 1)
    if cmd == "unpack" and len(sys.argv) == 4:
        return cmd_unpack(sys.argv[2], sys.argv[3])
    if cmd == "pack":
        ap = argparse.ArgumentParser(prog="samsung-bootimg.py pack")
        num = lambda s: int(s, 0)
        for k in ("kernel", "ramdisk", "dt"):
            ap.add_argument("--" + k, required=True)
        ap.add_argument("--second")
        for k in ("pagesize", "kernel_addr", "ramdisk_addr", "second_addr", "tags_addr", "os_version_raw"):
            ap.add_argument("--" + k, type=num, required=True)
        ap.add_argument("--board", default="")
        ap.add_argument("--cmdline", default="")
        ap.add_argument("-o", required=True)
        return cmd_pack(ap.parse_args(sys.argv[2:]))
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
