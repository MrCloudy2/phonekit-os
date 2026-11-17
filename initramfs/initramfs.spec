# phonepanel initramfs (see scripts/mkcpio.py for the format)
dir   /dev                 0755 0 0
nod   /dev/console         0600 0 0 c 5 1
nod   /dev/null            0666 0 0 c 1 3
nod   /dev/kmsg            0644 0 0 c 1 11
dir   /proc                0755 0 0
dir   /sys                 0755 0 0
dir   /tmp                 1777 0 0
dir   /run                 0755 0 0
dir   /config              0755 0 0
dir   /etc                 0755 0 0
dir   /bin                 0755 0 0

# PID 1: paneld (built by `make`)
file  /init                ${PANELD_BIN} 0755 0 0
file  /etc/phonepanel-version ${VERSION_FILE} 0644 0 0

# Device firmware imported locally (never committed), e.g. Wi-Fi blobs: devices/<dev>/prebuilt/firmware/
#   wifi/ -> /vendor/etc/wifi (Samsung SCSC), efs/mac.info -> the Wi-Fi MAC the driver reads
dir   /vendor              0755 0 0
dir   /vendor/etc          0755 0 0
dir   /mnt                 0755 0 0
dir   /mnt/vendor          0755 0 0
dir   /mnt/vendor/efs      0755 0 0
dir   /mnt/vendor/efs/wifi 0755 0 0
? tree  /vendor/etc/wifi     ${FW_DIR}/wifi 0644 0 0
? file  /mnt/vendor/efs/wifi/.mac.info ${FW_DIR}/efs/mac.info 0644 0 0
dir   /mnt/vendor/efs/bluetooth 0755 0 0
? file  /mnt/vendor/efs/bluetooth/bt_addr ${FW_DIR}/efs/bt_addr 0644 0 0

# Optional Wi-Fi userspace (scripts/build-wifi-tools.sh): wpa_supplicant/wpa_cli (WPA2-Personal), iw
? file  /bin/wpa_supplicant  ${WIFI_DIR}/wpa_supplicant 0755 0 0
? file  /bin/wpa_cli         ${WIFI_DIR}/wpa_cli 0755 0 0
? file  /bin/iw              ${WIFI_DIR}/iw 0755 0 0
? file  /bin/iptables        ${WIFI_DIR}/iptables 0755 0 0
? file  /bin/dropbearmulti   ${WIFI_DIR}/dropbearmulti 0755 0 0

# Optional debug shell: `make BUSYBOX=/path/to/static/aarch64/busybox`
? file  /bin/busybox       ${BUSYBOX} 0755 0 0
