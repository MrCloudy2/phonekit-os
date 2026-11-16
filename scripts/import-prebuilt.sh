#!/usr/bin/env bash
# Import a known-good kernel + DTB (+ recovery DTBO) from an existing boot or
# recovery image (e.g. a LineageOS/TWRP build for the device).
#
# usage: scripts/import-prebuilt.sh <device> <boot-or-recovery.img>
#
# Writes devices/<device>/prebuilt/:
#   kernel, dtb, [recovery_dtbo], mkbootimg.args (header layout without the ramdisk),
#   source.txt (where it came from + sha256)
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }
[[ $# -eq 2 ]] || die "usage: $0 <device> <image>"
device=$1 img=$2
root=$(cd "$(dirname "$0")/.." && pwd)
devdir=$root/devices/$device
[[ -d $devdir ]] || die "unknown device '$device' (no $devdir)"
[[ -f $img ]] || die "no such image: $img"
command -v unpack_bootimg >/dev/null || die "unpack_bootimg not found (install mkbootimg / android-tools)"

dst=$devdir/prebuilt
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

args=$(unpack_bootimg --boot_img "$img" --out "$tmp" --format mkbootimg)
[[ -s $tmp/kernel ]] || die "image has no kernel"

rm -rf "$dst"; mkdir -p "$dst"
for f in kernel dtb recovery_dtbo; do
    [[ -s $tmp/$f ]] && cp "$tmp/$f" "$dst/$f"
done

# Store the header layout with paths made relative to prebuilt/ and the ramdisk
# argument removed; mkimage.sh supplies our own ramdisk.
python3 - "$tmp" "$dst/mkbootimg.args" <<'EOF' "$args"
import shlex, sys
tmp, out, args = sys.argv[1], sys.argv[2], sys.argv[3]
a = shlex.split(args)
res, i = [], 0
while i < len(a):
    k = a[i]
    v = a[i + 1] if i + 1 < len(a) and not a[i + 1].startswith("--") else None
    i += 2 if v is not None else 1
    if k == "--ramdisk" or k == "--cmdline" and v is None:
        continue
    if v is not None and v.startswith(tmp + "/"):
        v = "@PREBUILT@/" + v[len(tmp) + 1:]
    res += [k] + ([v] if v is not None else [])
open(out, "w").write(" ".join(shlex.quote(x) for x in res) + "\n")
EOF

{
    echo "imported: $(date -Iseconds)"
    echo "from:     $(basename "$img")"
    echo "sha256:   $(sha256sum "$img" | cut -d' ' -f1)"
    echo "kernel:   $(strings "$dst/kernel" | grep -m1 '^Linux version' || echo unknown)"
} > "$dst/source.txt"

echo "imported into $dst:"
ls -la "$dst"
echo
cat "$dst/mkbootimg.args"
