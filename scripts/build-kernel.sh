#!/usr/bin/env bash
# Build a device kernel from third_party/ with the host LLVM toolchain.
#
#   scripts/build-kernel.sh <device> [extra make targets]
#
# Reads from devices/<device>/device.mk:
#   KERNEL_SRC        directory under third_party/
#   KERNEL_DEFCONFIG  base defconfig
#   KERNEL_FRAGMENTS  config fragments (relative to devices/<device>/), merged on top
#   KERNEL_DTB_GLOB   glob (in the build's arch/arm64/boot/dts) of DTBs to concatenate into dtb
#   KERNEL_MAKE_FLAGS extra make variables for this tree (e.g. toolchain overrides an old Makefile needs)
# Output: build/kernel-<device>/ (objects), out/<device>/kernel/{Image,dtb,config}
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
device=${1:?device}; shift || true

getvar() { make -s --no-print-directory -f - "print-$1" <<EOF
include $root/devices/$device/device.mk
print-%: ; @echo \$(\$*)
EOF
}
src=$root/third_party/$(getvar KERNEL_SRC)
defconfig=$(getvar KERNEL_DEFCONFIG)
fragments=${KERNEL_FRAGMENTS-$(getvar KERNEL_FRAGMENTS)}  # env overrides device.mk
dtbglob=$(getvar KERNEL_DTB_GLOB)
read -r -a makeflags <<< "$(getvar KERNEL_MAKE_FLAGS)"
[[ -d $src ]] || { echo "no kernel source at $src (scripts/fetch.sh kernel)" >&2; exit 1; }

obj=$root/build/kernel-$device
out=$root/out/$device/kernel
mkdir -p "$obj" "$out"

# Reset the source tree to its pristine checkout, then apply exactly
# devices/<device>/kernel-patches/*.patch (patches moved elsewhere are thereby dropped).
git -C "$src" checkout -q -- .
for p in "$root"/devices/"$device"/kernel-patches/*.patch; do
    [[ -f $p ]] || continue
    git -C "$src" apply "$p" && echo "applied $(basename "$p")"
done

# Neutral, reproducible build identity (otherwise the kernel embeds this PC's user and hostname).
export KBUILD_BUILD_USER=phonepanel KBUILD_BUILD_HOST=build

kmake() {
    make -C "$src" O="$obj" ARCH=arm64 \
        CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump \
        STRIP=llvm-strip CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-linux-gnu- \
        KCFLAGS="${KCFLAGS:-}" "${makeflags[@]}" -j"$(nproc)" "$@"
}

kmake "$defconfig"
if [[ -n $fragments ]]; then
    frags=()
    for f in $fragments; do frags+=("$root/devices/$device/$f"); done
    (cd "$obj" && ARCH=arm64 "$src/scripts/kconfig/merge_config.sh" -m -O "$obj" "$obj/.config" "${frags[@]}" >/dev/null)
    kmake olddefconfig
fi
kmake Image dtbs "$@"

cp "$obj/arch/arm64/boot/Image" "$out/Image"
cp "$obj/.config" "$out/config"
if [[ -n $dtbglob ]]; then
    # shellcheck disable=SC2086
    cat $obj/arch/arm64/boot/dts/$dtbglob > "$out/dtb"
fi
ls -la "$out"
strings "$out/Image" | grep -m1 'Linux version' || true
