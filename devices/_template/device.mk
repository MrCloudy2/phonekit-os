# Build settings for <Brand Model> (<model number>, <soc>).
# Copy this folder to devices/<vendor>-<codename>/. Human-readable facts go in device.yaml.

ARCH            := arm64
CROSS_CC        ?= aarch64-linux-gnu-gcc

# Partition phonepanel is flashed to. RECOVERY keeps your ROM on boot (recommended).
TARGET_PARTITION      := RECOVERY            # heimdall partition name (Samsung) / informational otherwise
TARGET_PARTITION_NAME := recovery            # /dev/block/by-name/<this>
TARGET_PARTITION_SIZE := 0                   # bytes: adb shell su -c 'blockdev --getsize64 /dev/block/by-name/recovery'

# Known-good image to import the kernel/DTB/DTBO from. Put your local path in local.mk (gitignored)
# or pass IMG_SRC=… to make import.
PREBUILT_FROM ?= /path/to/recovery.img

# Append an AVB hash footer so the image is exactly partition-sized (most devices since Android 9).
AVB_FOOTER := 1

# 1 if the panel shows red and blue swapped (check with `scripts/ppctl.py colortest`).
FB_SWAP_RB := 0

# How to flash from the emergency mode. Samsung:
FLASH_CMD = heimdall flash --$(TARGET_PARTITION) $(1) --no-reboot
# fastboot devices:
# FLASH_CMD = fastboot flash $(TARGET_PARTITION_NAME) $(1)

# Optional: kernel from source (scripts/build-kernel.sh, `make KERNEL=source`).
# KERNEL_SRC       := <dir under third_party/>
# KERNEL_DEFCONFIG := <defconfig>
# KERNEL_FRAGMENTS := kernel/phonepanel.cfg
# KERNEL_DTB_GLOB  :=
