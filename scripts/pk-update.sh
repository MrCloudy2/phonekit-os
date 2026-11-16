#!/usr/bin/env bash
# Update PhoneKit from PhoneKit itself (M14): no ROM, no download mode.
#
#   scripts/pk-update.sh <image> [partition]      (make update)
#
# Uploads the image over the USB control channel (~35 MB/s), checks its sha256 on the phone, writes it to the
# partition PhoneKit runs from while keeping the settings store ([end-2 MiB, end-1 MiB)), reads it back to verify,
# then reboots into it. If anything fails before the write, nothing changed. If the write itself fails half-way,
# the ROM on the boot partition still boots and `make deploy` (or download mode) restores PhoneKit.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
img=${1:?image}; part=${2:-recovery}
ppctl() { "$here/ppctl.py" "$@"; }
[[ -f $img ]] || { echo "no such image $img" >&2; exit 1; }
size=$(stat -c %s "$img")
(( size % 65536 == 0 )) || { echo "image size $size is not a multiple of 64 KiB (needs the AVB-padded, partition-sized image)" >&2; exit 1; }
want=$(sha256sum "$img" | cut -d' ' -f1)
nblk=$(( size / 65536 )); s0=$(( nblk - 32 )); s1=$(( nblk - 16 ))
want_kept=$( { head -c $(( s0 * 65536 )) "$img"; tail -c $(( (nblk - s1) * 65536 )) "$img"; } | sha256sum | cut -d' ' -f1)

echo "pk-update: uploading $(basename "$img") ($(( size / 1048576 )) MiB)"
ppctl --wait 60 put "$img" /tmp/update.img >/dev/null

tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
cat > "$tmp" <<EOF
set -e
blk=""
for d in /sys/class/block/*; do grep -qx "PARTNAME=$part" \$d/uevent 2>/dev/null && blk=/dev/\$(basename \$d); done
[ -n "\$blk" ] || { echo "no partition named $part"; exit 2; }
[ -b "\$blk" ] || mknod "\$blk" b \$(cut -d: -f1 /sys/class/block/\$(basename \$blk)/dev) \$(cut -d: -f2 /sys/class/block/\$(basename \$blk)/dev)
psize=\$(blockdev --getsize64 \$blk)
[ "\$psize" = "$size" ] || { echo "partition \$blk is \$psize bytes, image is $size"; exit 3; }
got=\$(sha256sum /tmp/update.img | cut -d' ' -f1)
[ "\$got" = "$want" ] || { echo "upload corrupted (\$got)"; exit 4; }
dd if=/tmp/update.img of=\$blk bs=65536 count=$s0 conv=fsync 2>/dev/null
dd if=/tmp/update.img of=\$blk bs=65536 skip=$s1 seek=$s1 conv=fsync 2>/dev/null
sync
echo 3 > /proc/sys/vm/drop_caches
back=\$( { dd if=\$blk bs=65536 count=$s0 2>/dev/null; dd if=\$blk bs=65536 skip=$s1 2>/dev/null; } | sha256sum | cut -d' ' -f1)
rm -f /tmp/update.img
[ "\$back" = "$want_kept" ] || { echo "VERIFY FAILED: \$blk reads back \$back"; exit 5; }
echo "written and verified \$blk (settings store kept)"
EOF
ppctl put "$tmp" /tmp/pk-update.sh 755 >/dev/null
out=$(ppctl runt 120 /bin/sh /tmp/pk-update.sh 2>&1) || { echo "pk-update: $out" >&2; exit 1; }
echo "pk-update: $out"
echo "pk-update: rebooting into the new image"
ppctl reboot recovery >/dev/null 2>&1 || true
