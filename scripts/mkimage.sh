#!/usr/bin/env bash
# Assemble the flashable phonepanel image for a device.
#
# usage: scripts/mkimage.sh <device> <ramdisk.cpio.gz> <out.img>
# Reads devices/<device>/device.mk (via make) and devices/<device>/prebuilt/.
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }
[[ $# -eq 3 ]] || die "usage: $0 <device> <ramdisk> <out.img>"
device=$1 ramdisk=$2 out=$3
# Never leave a half-checked image behind where `make flash` could pick it up.
trap 'rc=$?; (( rc == 0 )) || rm -f "$out"' EXIT
root=$(cd "$(dirname "$0")/.." && pwd)
pre=$root/devices/$device/prebuilt
[[ -f $pre/mkbootimg.args ]] || die "no prebuilt kernel for $device; run: make import device=$device"

# Pull variables from device.mk
getvar() { make -s --no-print-directory -f - "print-$1" <<EOF
include $root/devices/$device/device.mk
print-%: ; @echo \$(\$*)
EOF
}
part_name=$(getvar TARGET_PARTITION_NAME)
part_size=$(getvar TARGET_PARTITION_SIZE)
avb=$(getvar AVB_FOOTER)

eval "args=($(sed "s|@PREBUILT@|$pre|g" "$pre/mkbootimg.args"))"
# KERNEL_IMAGE=path swaps in a kernel built from source (DTB/DTBO stay prebuilt)
if [[ -n ${KERNEL_IMAGE:-} ]]; then
    [[ -f $KERNEL_IMAGE ]] || die "KERNEL_IMAGE $KERNEL_IMAGE not found"
    for i in "${!args[@]}"; do [[ ${args[$i]} == --kernel ]] && args[$((i + 1))]=$KERNEL_IMAGE; done
    echo "using kernel $KERNEL_IMAGE"
fi
if [[ " ${args[*]} " == *" --dt "* ]]; then   # Samsung header v0 + DT (see scripts/samsung-bootimg.py)
    python3 "$root/scripts/samsung-bootimg.py" pack "${args[@]}" --ramdisk "$ramdisk" -o "$out"
else
    mkbootimg "${args[@]}" --ramdisk "$ramdisk" -o "$out"
fi

size=$(stat -c %s "$out")
(( part_size > 0 )) || die "TARGET_PARTITION_SIZE unknown for $device: read it from the phone (blockdev --getsize64) first"
# Headroom: paneld's settings store lives in [end-2MiB, end-1MiB) of the partition (with or without AVB),
# and the AVB footer, when used, at the end. The payload (plus its vbmeta) must stay below the store.
(( size + 2 * 1048576 + 65536 <= part_size )) || die "image ($size B) too large for $part_name ($part_size B) incl. settings store"
if [[ $avb == 1 ]]; then
    # Salt derived from the payload so identical inputs give identical images.
    avbtool add_hash_footer --image "$out" --partition_name "$part_name" \
        --partition_size "$part_size" --algorithm NONE \
        --salt "$(sha256sum "$out" | cut -d' ' -f1)"
fi
if [[ $avb != 1 ]]; then
    # No AVB footer: pad with zeros to the partition size anyway, so every image is partition-sized and
    # phone.sh / pk-update.sh can skip paneld's settings store when they write it. The bootloader only reads
    # the sections the header describes.
    truncate -s "$part_size" "$out"
fi
(( $(stat -c %s "$out") <= part_size )) || die "image too large for $part_name"
echo "built $out ($size B payload, partition $part_name $part_size B)"
