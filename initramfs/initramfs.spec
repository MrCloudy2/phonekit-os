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

# Optional debug shell: `make BUSYBOX=/path/to/static/aarch64/busybox`
? file  /bin/busybox       ${BUSYBOX} 0755 0 0
