#!/usr/bin/env bash
# Build a static aarch64 busybox from third_party/busybox-<ver> into out/busybox/busybox.
# Fetch first:  scripts/fetch.sh busybox
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=$(ls -d "$root"/third_party/busybox-*/ 2>/dev/null | sort -V | tail -1)
[[ -n $src ]] || { echo "no busybox source in third_party/ (run scripts/fetch.sh busybox)" >&2; exit 1; }
obj=$root/build/busybox
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p "$obj" "$root/out/busybox"

make -s -C "$src" O="$obj" ARCH=arm64 CROSS_COMPILE="$cross" defconfig >/dev/null
cfg=$obj/.config
set_opt() { sed -i "/^# $1 is not set/d; /^$1=/d" "$cfg"; echo "$1=$2" >> "$cfg"; }
off_opt() { sed -i "/^$1=/d" "$cfg"; echo "# $1 is not set" >> "$cfg"; }
set_opt CONFIG_STATIC y
off_opt CONFIG_TC            # needs kernel CBQ headers that newer kernels dropped
off_opt CONFIG_FEATURE_TC_INGRESS
off_opt CONFIG_SHA1_HWACCEL   # x86-only code paths, break the arm64 build in 1.37
off_opt CONFIG_SHA256_HWACCEL
make -s -C "$src" O="$obj" ARCH=arm64 CROSS_COMPILE="$cross" oldconfig </dev/null >/dev/null
make -s -C "$src" O="$obj" ARCH=arm64 CROSS_COMPILE="$cross" -j"$(nproc)" busybox
"${cross}strip" -o "$root/out/busybox/busybox" "$obj/busybox"
file "$root/out/busybox/busybox"
