# Build settings for the Samsung Galaxy XCover Pro (SM-G715FN, exynos9611).
# Human-readable facts live in device.yaml; this file is what the build reads.

ARCH            := arm64
CROSS_CC        ?= aarch64-linux-gnu-gcc

# Where phonepanel gets flashed. Using the recovery partition keeps the normal
# boot partition (LineageOS) untouched: power on -> LineageOS, Vol Up + Power
# (or `adb reboot recovery`) -> phonepanel.
TARGET_PARTITION      := RECOVERY
TARGET_PARTITION_NAME := recovery
TARGET_PARTITION_SIZE := 71106560

# Prebuilt kernel source image, imported with
#   scripts/import-prebuilt.sh samsung-xcoverpro <this file>
PREBUILT_FROM ?= /path/to/lineage-recovery.img   # or pass IMG_SRC=… to make import

# Samsung bootloader: unlocked, AVB errors are tolerated (orange state).
# We append an unsigned AVB hash footer so the partition layout matches what
# the bootloader expects.
AVB_FOOTER := 1

# The panel displays the framebuffer as BGR although fbdev reports R at bit 16
# (verified by eye with `ppctl colortest`, 2026-11-15).

FLASH_CMD = heimdall flash --$(TARGET_PARTITION) $(1) --no-reboot

# Kernel from source (scripts/build-kernel.sh; `make KERNEL=source`). DTB/DTBO stay prebuilt.
KERNEL_SRC       := kernel-universal9611
KERNEL_DEFCONFIG := exynos9611-xcoverpro_defconfig
KERNEL_FRAGMENTS := kernel/phonepanel.cfg
KERNEL_DTB_GLOB  :=
