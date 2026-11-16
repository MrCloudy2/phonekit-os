#!/usr/bin/env bash
# Static aarch64 Wi-Fi userspace for the initramfs: libnl 3 → wpa_supplicant (+ wpa_cli) and iw.
#
#   scripts/build-wifi-tools.sh        (sources downloaded into third_party/, see below)
#
# Output: out/wifi/{wpa_supplicant,wpa_cli,iw}. wpa_supplicant uses its internal TLS/crypto, so no OpenSSL is
# needed: WPA2-Personal only for now (WPA3/SAE needs EC crypto, i.e. OpenSSL or wolfSSL; later).
# Sources (sha256 in third_party/wifi-tools.sha256):
#   https://github.com/thom311/libnl/releases/download/libnl3_12_0/libnl-3.12.0.tar.gz
#   https://w1.fi/releases/wpa_supplicant-2.11.tar.gz
#   https://www.kernel.org/pub/software/network/iw/iw-6.9.tar.xz
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tp=$root/third_party
cd "$tp"
sha256sum -c wifi-tools.sha256
CC=${CROSS_CC:-aarch64-linux-gnu-gcc}
HOST=aarch64-linux-gnu
pfx=$root/build/wifi-sysroot
out=$root/out/wifi
mkdir -p "$pfx" "$out"
jobs=$(nproc)

# libnl (static, genl + route are what wpa_supplicant/iw use)
[[ -d libnl-3.12.0 ]] || tar xf libnl-3.12.0.tar.gz
( cd libnl-3.12.0
  [[ -f .pp-built ]] || {
    ./configure --host=$HOST CC="$CC" --prefix="$pfx" --enable-static --disable-shared --disable-cli --disable-debug >/dev/null
    make -j"$jobs" >/dev/null && make install >/dev/null && touch .pp-built; } )

export PKG_CONFIG_PATH=$pfx/lib/pkgconfig PKG_CONFIG_LIBDIR=$pfx/lib/pkgconfig

# wpa_supplicant
[[ -d wpa_supplicant-2.11 ]] || tar xf wpa_supplicant-2.11.tar.gz
( cd wpa_supplicant-2.11/wpa_supplicant
  cat > .config <<'EOF'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_INTERNAL_LIBTOMMATH_FAST=y
CONFIG_CRYPTO=internal
CONFIG_IEEE80211W=y
CONFIG_IEEE80211N=y
CONFIG_IEEE80211AC=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_BACKEND=file
EOF
  make clean >/dev/null 2>&1 || true
  make -j"$jobs" CC="$CC" \
       EXTRA_CFLAGS="-I$pfx/include/libnl3 -Os" LDFLAGS="-static -L$pfx/lib" LIBS="-lnl-genl-3 -lnl-3 -lpthread -lm" \
       wpa_supplicant wpa_cli >/dev/null
  cp wpa_supplicant wpa_cli "$out/" )

# iw
[[ -d iw-6.9 ]] || tar xf iw-6.9.tar.xz
( cd iw-6.9
  make clean >/dev/null 2>&1 || true
  make -j"$jobs" CC="$CC" PKG_CONFIG=pkg-config NLLIBNAME=libnl-3.0 \
       CFLAGS="-Os -I$pfx/include/libnl3 -DCONFIG_LIBNL30" LDFLAGS="-static -L$pfx/lib" LIBS="-lnl-genl-3 -lnl-3 -lpthread -lm" >/dev/null
  cp iw "$out/" )

${CROSS_STRIP:-aarch64-linux-gnu-strip} "$out"/*
ls -la "$out"
