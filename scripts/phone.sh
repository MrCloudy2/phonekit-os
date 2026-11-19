#!/usr/bin/env bash
# Hands-free phone loop over adb (needs a userdebug ROM with "Rooted debugging").
#
#   scripts/phone.sh flash <img> [partition]  dd an image onto a partition from the running ROM
#   scripts/phone.sh boot                     reboot into the phonepanel partition (recovery)
#   scripts/phone.sh wait                     wait until the ROM has booted and adb root is up
#   scripts/phone.sh logs [out]               save phonepanel lines from the previous boot's kernel log
#   scripts/phone.sh backup <out.img> [part]  read a partition into a file
#
# All commands that touch partitions verify by sha256 afterwards.
set -euo pipefail

die() { echo "phone: $*" >&2; exit 1; }
log() { echo "phone: $*" >&2; }
PART_DEFAULT=recovery

# PhoneKit's own adbd (PHONEBOX mode) also shows up as an adb "device": tell it apart and send it to the ROM.
is_phonekit() {
    [[ $(adb shell getprop ro.phonekit 2>/dev/null | tr -d '\r') == 1 ]] || return 1
    if [[ -z ${PK_REBOOTED:-} ]]; then log "PhoneKit is running: rebooting it into the ROM"; adb reboot >/dev/null 2>&1 || true; PK_REBOOTED=1; fi
    return 0
}

wait_rom() {
    local t=${1:-240} i=0
    log "waiting for the ROM (adb)…"
    until [[ $(adb get-state 2>/dev/null) == device ]] && ! is_phonekit &&
          [[ $(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') == 1 ]]; do
        sleep 2; (( (i += 2) < t )) || die "ROM didn't come up within ${t}s"
    done
    adb root >/dev/null 2>&1 || true
    sleep 2; adb wait-for-device
    [[ $(adb shell id -u | tr -d '\r') == 0 ]] || die "adb root failed (enable Rooted debugging)"
    # Our image only survives if the ROM doesn't restore recovery on boot.
    if [[ $(adb shell getprop persist.vendor.recovery_update | tr -d '\r') == true ]]; then
        log "turning off persist.vendor.recovery_update (it would overwrite our image)"
        adb shell setprop persist.vendor.recovery_update false
    fi
}

# Block device for a partition name. Older Samsung ROMs and their TWRP only have
# /dev/block/platform/<storage>/by-name/ (e.g. Galaxy S8: 11120000.ufs).
blk_of() {
    local b
    b=$(adb shell "for d in /dev/block/by-name/$1 /dev/block/platform/*/by-name/$1 /dev/block/bootdevice/by-name/$1; do [ -e \$d ] && { echo \$d; break; }; done" | tr -d '\r')
    [[ -n $b ]] || die "no block device for partition '$1'"
    echo "$b"
}
part_sha() { adb exec-out "dd if=$(blk_of "$1") bs=1M 2>/dev/null" | sha256sum | cut -d' ' -f1; }

cmd=${1:-}; shift || true
case $cmd in
wait) wait_rom "${1:-240}" ;;
flash)
    img=${1:?image}; part=${2:-$PART_DEFAULT}
    [[ -f $img ]] || die "no such image $img"
    wait_rom
    blk=$(blk_of "$part")
    psize=$(adb shell blockdev --getsize64 "$blk" | tr -d '\r')
    isize=$(stat -c %s "$img")
    (( isize <= psize )) || die "image ($isize) larger than $part ($psize)"
    log "writing $(basename "$img") → $part"
    adb push -q "$img" /data/local/tmp/pp.img
    if (( isize == psize && psize % 65536 == 0 )); then
        # Full-partition image (AVB-padded): skip paneld's settings store at
        # [end-2MiB, end-1MiB) so a deploy keeps the phone's settings.
        nblk=$(( psize / 65536 )); s0=$(( nblk - 32 )); s1=$(( nblk - 16 ))
        adb shell "dd if=/data/local/tmp/pp.img of=$blk bs=65536 count=$s0 conv=fsync 2>/dev/null &&
                   dd if=/data/local/tmp/pp.img of=$blk bs=65536 skip=$s1 seek=$s1 conv=fsync 2>/dev/null &&
                   rm /data/local/tmp/pp.img && sync"
        want=$( { head -c $(( s0 * 65536 )) "$img"; tail -c $(( (nblk - s1) * 65536 )) "$img"; } | sha256sum | cut -d' ' -f1)
        got=$(adb exec-out "dd if=$blk bs=65536 count=$s0 2>/dev/null; dd if=$blk bs=65536 skip=$s1 2>/dev/null" | sha256sum | cut -d' ' -f1)
        what="(settings store preserved)"
    else
        adb shell "dd if=/data/local/tmp/pp.img of=$blk bs=1M conv=fsync 2>/dev/null && rm /data/local/tmp/pp.img && sync"
        want=$(sha256sum "$img" | cut -d' ' -f1)
        got=$(adb exec-out "dd if=$blk bs=1M count=$(( (isize + 1048575) / 1048576 )) 2>/dev/null" | head -c "$isize" | sha256sum | cut -d' ' -f1)
        what=""
    fi
    [[ $got == "$want" ]] || die "verify failed: $part has $got, expected $want"
    log "verified $part = $want $what"
    ;;
boot)
    wait_rom 60
    log "rebooting into recovery (phonepanel)"
    adb reboot recovery
    ;;
logs)
    out=${1:-/dev/stdout}
    wait_rom
    tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
    adb exec-out 'cat /proc/last_kmsg' > "$tmp"
    # last_kmsg can hold several boots; keep the last one that ran paneld.
    # (one awk pass: a `tail | grep -q` loop breaks under pipefail via SIGPIPE)
    start=$(awk '/Linux version/ { boot = NR } /paneld:/ && boot { found = boot } END { print found }' "$tmp")
    [[ -n $start ]] || die "no paneld boot found in last_kmsg"
    tail -n +"$start" "$tmp" | grep -a -E 'Linux version|paneld|Kernel panic|Oops|watchdog|BUG:' |
        sed 's/^<[0-9]*>//' | cut -c1-220 > "$out"
    ;;
backup)
    out=${1:?out}; part=${2:-$PART_DEFAULT}
    wait_rom
    blk=$(blk_of "$part")
    adb exec-out "dd if=$blk bs=1M 2>/dev/null" > "$out"
    log "$part → $out ($(sha256sum "$out" | cut -c1-16)…)"
    ;;
*) sed -n '2,10p' "$0"; exit 1 ;;
esac
