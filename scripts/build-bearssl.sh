#!/usr/bin/env bash
# BearSSL 0.6 (MIT, https://bearssl.org/) as a static aarch64 library for paneld's TLS client (PHONEBOX over Wi-Fi).
#   scripts/build-bearssl.sh   -> out/bearssl/libbearssl.a, out/bearssl/inc/
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/third_party"
[[ -f bearssl-0.6.tar.gz ]] || curl -fsSL -o bearssl-0.6.tar.gz https://bearssl.org/bearssl-0.6.tar.gz
sha256sum -c bearssl.sha256
[[ -d bearssl-0.6 ]] || tar xzf bearssl-0.6.tar.gz
out=$root/out/bearssl obj=$root/build/bearssl
mkdir -p "$out" "$obj"
CC=${CROSS_CC:-aarch64-linux-gnu-gcc}
objs=()
for c in $(find bearssl-0.6/src -name '*.c' | sort); do
    o=$obj/$(echo "${c#bearssl-0.6/src/}" | tr / _).o
    [[ $o -nt $c ]] || $CC -Os -fPIC -I bearssl-0.6/inc -I bearssl-0.6/src -c "$c" -o "$o"
    objs+=("$o")
done
rm -f "$out/libbearssl.a"
${CROSS_AR:-aarch64-linux-gnu-ar} rcs "$out/libbearssl.a" "${objs[@]}"
rm -rf "$out/inc" && cp -r bearssl-0.6/inc "$out/inc"
ls -la "$out/libbearssl.a"
