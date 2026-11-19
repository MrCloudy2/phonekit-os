#!/bin/sh
# PhoneKit hardware probe (M16): run ON THE PHONE in any root shell (PhoneKit, a TWRP/LineageOS recovery, or
# `adb root` on a ROM); prints a device.yaml draft with what it finds. Read-only: it changes nothing.
#
#   adb push scripts/probe.sh /tmp/ && adb shell sh /tmp/probe.sh > draft.yaml
#   scripts/ppctl.py put scripts/probe.sh /tmp/probe.sh && scripts/ppctl.py run /bin/sh /tmp/probe.sh
#
# Busybox/toybox sh compatible. Fields it can't determine are left empty with a "# TODO".

p() { printf '%s\n' "$*"; }
rd() { [ -r "$1" ] && head -c 200 "$1" 2>/dev/null | tr -d '\0' | head -1; }
prop() {   # Android property, or the bootloader's androidboot.* / device tree when there is no getprop
    v=""; command -v getprop >/dev/null 2>&1 && v=$(getprop "$1" 2>/dev/null)
    [ -n "$v" ] && { printf '%s' "$v"; return; }
    case "$1" in
    ro.product.model) v=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^androidboot.em.model=//p;s/^androidboot.model=//p' | head -1) ;;
    ro.product.device) v=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^androidboot.device=//p;s/^androidboot.hardware=//p' | head -1) ;;
    ro.product.marketname) v=$(tr -d '\0' < /sys/firmware/devicetree/base/model 2>/dev/null) ;;
    esac
    printf '%s' "$v"
}

p "# PhoneKit probe $(date -u +%Y-%m-%dT%H:%MZ 2>/dev/null) — review every value before committing"
p "schema: 0"
p ""
p "identity:"
p "  name: $(prop ro.product.marketname || prop ro.product.model)        # TODO marketing name"
p "  model: $(prop ro.product.model)"
p "  codename: $(prop ro.product.device)"
soc=$(grep -m1 -iE '^Hardware' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')
[ -z "$soc" ] && soc=$(rd /sys/devices/soc0/machine)
p "  soc: ${soc:-}        # TODO if empty"
p "  cpus: $(grep -c ^processor /proc/cpuinfo 2>/dev/null)"
p "  ram_mb: $(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1024 ))"
p "  kernel: \"$(uname -r)\""
p ""

p "display:"
if [ -e /dev/fb0 ] || [ -d /sys/class/graphics/fb0 ]; then
    p "  api: fbdev"
    p "  node: /dev/fb0"
    p "  name: \"$(rd /sys/class/graphics/fb0/name)\""
    p "  virtual_size: \"$(rd /sys/class/graphics/fb0/virtual_size)\"   # double buffer if height = 2x panel"
    p "  bits_per_pixel: $(rd /sys/class/graphics/fb0/bits_per_pixel)"
    p "  stride: $(rd /sys/class/graphics/fb0/stride)"
fi
if ls /sys/class/drm/card*-* >/dev/null 2>&1; then
    p "  drm_connectors:"
    for c in /sys/class/drm/card*-*; do p "    - \"$(basename "$c") $(rd "$c/status") $(head -1 "$c/modes" 2>/dev/null)\""; done
fi
bl=$(ls -d /sys/class/backlight/* 2>/dev/null | head -1)
p "  backlight: \"${bl:-}\"   # max $(rd "$bl/max_brightness")"
p "  color_order_verified: false   # check with ppctl colortest; set fb_swap_rb in hw.c if red shows blue"
p ""

p "input:"
for e in /sys/class/input/event*; do
    n=$(rd "$e/device/name"); [ -z "$n" ] && continue
    abs=$(rd "$e/device/capabilities/abs"); key=$(rd "$e/device/capabilities/key")
    kind=other
    case "$abs" in *[1-9a-f]*) [ ${#abs} -gt 8 ] && kind=touch ;; esac   # multitouch bits are high in the abs mask
    [ "$kind" = other ] && [ -n "$key" ] && [ "$key" != 0 ] && kind=keys
    p "  - { node: $(basename "$e"), name: \"$n\", kind: $kind }"
done
p ""

p "usb:"
p "  udc: $(ls /sys/class/udc 2>/dev/null | head -1)"
p "  configfs: $( [ -d /config/usb_gadget ] || [ -d /sys/kernel/config/usb_gadget ] && echo true || echo false)   # mount -t configfs none /config if false"
p "  speed_max: \"$(rd /sys/class/udc/$(ls /sys/class/udc 2>/dev/null | head -1)/maximum_speed)\""
p ""

p "power:"
for s in /sys/class/power_supply/*; do
    t=$(rd "$s/type"); n=$(basename "$s")
    extra=""
    for f in capacity charging_enabled batt_slate_mode input_suspend charge_control_limit store_mode current_now voltage_now temp; do
        [ -e "$s/$f" ] && extra="$extra $f"
    done
    p "  - { name: $n, type: $t, files: \"${extra# }\" }"
done
p "  charge_limit_method: \"\"   # TODO: the file that stops charging (charging_enabled, input_suspend, store_mode, ...)"
p ""

p "storage:"
for d in /sys/class/block/*; do
    pn=$(grep PARTNAME "$d/uevent" 2>/dev/null | cut -d= -f2)
    [ -n "$pn" ] || continue
    case "$pn" in boot|boot_a|boot_b|recovery|vendor_boot*|super|vbmeta*|dtbo*|cache|userdata|efs|persist|misc)
        p "  - { name: $pn, dev: $(basename "$d"), size: $(( $(cat "$d/size") * 512 )) }" ;;
    esac
done
p ""

p "sensors:"
if [ -d /sys/class/sensors ]; then p "  samsung_ssp: $(ls /sys/class/sensors | tr '\n' ' ')"; fi
for i in /sys/bus/iio/devices/iio:device*; do [ -e "$i/name" ] && p "  - iio: \"$(rd "$i/name")\""; done
p ""

p "wifi:"
ifc=$(ls /sys/class/net 2>/dev/null | grep -m1 -E '^wlan')
p "  interface: ${ifc:-}"
[ -n "$ifc" ] && p "  driver: \"$(basename "$(readlink /sys/class/net/$ifc/device/driver 2>/dev/null)")\""
for fw in /vendor/etc/wifi /vendor/firmware /lib/firmware /system/etc/wifi; do
    [ -d "$fw" ] && p "  firmware_dir_candidate: $fw ($(ls "$fw" | wc -l) files)"
done
p ""
p "quirks: []   # add every surprise here (see socs/<soc>/quirks.md)"
