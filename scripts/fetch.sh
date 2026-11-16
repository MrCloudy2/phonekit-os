#!/usr/bin/env bash
# Download third-party sources into third_party/ (git-ignored).
#   scripts/fetch.sh busybox      busybox release tarball (sha256 verified)
#   scripts/fetch.sh kernel       XCover Pro LineageOS kernel fork (shallow)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/third_party"
cd "$root/third_party"

BUSYBOX_VER=1.37.0
KERNEL_URL=https://github.com/MrCloudy2/android_kernel_samsung_universal9611
KERNEL_BRANCH=xcoverpro-24.0

for what in "$@"; do
    case $what in
    busybox)
        f=busybox-$BUSYBOX_VER.tar.bz2
        [[ -d busybox-$BUSYBOX_VER ]] && continue
        curl -sSfLO "https://busybox.net/downloads/$f"
        curl -sSfLO "https://busybox.net/downloads/$f.sha256"
        sha256sum -c "$f.sha256"
        tar xjf "$f"
        ;;
    kernel)
        [[ -d kernel-universal9611 ]] && continue
        git clone --depth 1 -b "$KERNEL_BRANCH" "$KERNEL_URL" kernel-universal9611
        ;;
    *) echo "unknown: $what" >&2; exit 1 ;;
    esac
done
