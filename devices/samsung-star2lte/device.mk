# Build settings for the Samsung Galaxy S9+ (SM-G965F, exynos9810). Port in progress.
# Human-readable facts and their sources live in device.yaml; how paneld drives the hardware in hw.c.
# Only the Exynos model. The Snapdragon S9+ (SM-G965U/U1/W) can't unlock its bootloader.

ARCH            := arm64
CROSS_CC        ?= aarch64-linux-gnu-gcc

# Upper case, as on the S8: /dev/block/by-name/RECOVERY -> sda11 (LineageOS 23.2, 2026-11-19)
TARGET_PARTITION      := RECOVERY
TARGET_PARTITION_NAME := RECOVERY
# blockdev --getsize64 /dev/block/by-name/RECOVERY on the phone (2026-11-19); LineageOS's
# BOARD_RECOVERYIMAGE_PARTITION_SIZE says the same
TARGET_PARTITION_SIZE := 68149248

# Known-good image: the LineageOS recovery.img of the ROM on the phone (its kernel and DT table match the ROM).
# The ROM's maintainer supports only Lineage Recovery, not TWRP. Put your local path in local.mk (gitignored)
# or pass IMG_SRC=… to make import.
# Samsung header v0 + DT layout, handled by scripts/samsung-bootimg.py.
PREBUILT_FROM ?= /path/to/lineage-24.0-20261112-recovery-star2lte.img

# Launched on Android 8 with Samsung's own signing, no AVB: the LineageOS images have no footer.
AVB_FOOTER := 0

# Download mode: phone off, hold Vol Down + Bixby + Power, then Vol Up to continue.
FLASH_CMD = heimdall flash --$(TARGET_PARTITION) $(1) --no-reboot

# Kernel from source (scripts/build-kernel.sh): the exact tree of the LineageOS 24.0 (2026-11-12) kernel,
# github.com/ExyHyperBrick/android_kernel_samsung_exynos9810 at 401189fdfc65 (branch lineage-24.0, 4.9.337,
# LOCALVERSION -ies). Clone it into third_party/kernel-exynos9810. The DT table stays prebuilt: import the matching
# LineageOS recovery.img. Without the source tree, build with KERNEL=prebuilt (no HID gadget, no devtmpfs).
KERNEL_SRC       := kernel-exynos9810
KERNEL_DEFCONFIG := exynos9810-star2lte_defconfig
KERNEL_FRAGMENTS := kernel/phonepanel.cfg
KERNEL_DTB_GLOB  :=
# The tree is built with Android clang 21. Host clang 23 adds diagnostics that -Werror turns into errors, all for
# patterns that are fine here: -Wdefault-const-init-*-unsafe (bpf test_run.c, v4l2-ioctl.c; upstream Linux turned
# it off in 2025), -Wunused-but-set-global (bcmdhd dhd_debug.c) and -Wframe-larger-than (exynos-flexpmu-dbg.c keeps a
# debugfs buffer on the stack). bcmdhd re-enables its warnings after KCFLAGS, so -Wno-error=, not -Wno-.
KERNEL_KCFLAGS := -Wno-default-const-init-unsafe -Wno-error=unused-but-set-global -Wno-error=frame-larger-than
