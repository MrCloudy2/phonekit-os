#!/bin/sh
# Runs ON THE PHONE (busybox sh): measure system power draw from the fuel gauge.
#   power-measure.sh <seconds> [label]
# Puts the charger in slate mode (USB input suspended, phone runs from battery, so the
# gauge reads true system draw), samples current/voltage every 0.5 s, restores charging.
B=/sys/class/power_supply/battery
secs=${1:-10}; label=${2:-sample}
prev=$(cat $B/batt_slate_mode)
echo 1 > $B/batt_slate_mode
sleep 3                       # let the gauge settle after the switch
n=0; sum_ua=0; sum_uv=0; t_end=$(( $(date +%s) + secs ))
s1=$(awk '{print $1+$2+$3+$4+$5+$6+$7}' /proc/stat 2>/dev/null; head -1 /proc/stat)
while [ "$(date +%s)" -lt "$t_end" ]; do
    ua=$(cat $B/batt_current_ua_now 2>/dev/null || echo $(( $(cat $B/current_now) * 1000 )))
    uv=$(cat $B/voltage_now)
    sum_ua=$((sum_ua + ua)); sum_uv=$((sum_uv + uv)); n=$((n + 1))
    sleep 0.5
done
echo "$prev" > $B/batt_slate_mode
avg_ua=$((sum_ua / n)); avg_uv=$((sum_uv / n))
# discharge is negative on this gauge; report draw as a positive number
[ $avg_ua -lt 0 ] && avg_ua=$((-avg_ua))
mw=$(( avg_ua / 1000 * (avg_uv / 1000) / 1000 ))
echo "$label: ${n} samples, $((avg_ua / 1000)) mA @ $((avg_uv / 1000)) mV = ${mw} mW"
