# Build settings for the Samsung Galaxy S10e (SM-G970F, exynos9820). Port in progress.
# Human-readable facts and their sources live in device.yaml; how paneld drives the hardware in hw.c.
# Only the Exynos model (SM-G970F, SM-G970F/DS, SM-G970N). The Snapdragon S10e (SM-G970U/W) can't unlock.

ARCH            := arm64
CROSS_CC        ?= aarch64-linux-gnu-gcc

# Lower case: /dev/block/by-name/recovery -> sda15 (LineageOS 23.2, 2026-11-19); heimdall's PIT name is RECOVERY
TARGET_PARTITION      := RECOVERY
TARGET_PARTITION_NAME := recovery
# blockdev --getsize64 /dev/block/by-name/recovery on the phone (2026-11-19) = PIT 16512 x 4 KiB = LineageOS's
# BOARD_RECOVERYIMAGE_PARTITION_SIZE
TARGET_PARTITION_SIZE := 67633152

# Known-good image: the official LineageOS recovery.img of the ROM on the phone (same kernel as the ROM's boot.img).
# Put your local path in local.mk (gitignored) or pass IMG_SRC=… to make import.
# Plain AOSP header v1 with a recovery DTBO (unpack_bootimg reads it); the DTB lives in its own DTB partition.
PREBUILT_FROM ?= /path/to/lineage-23.2-20261114-recovery-beyond0lte.img

# Launched on Android 9 with AVB: LineageOS's recovery carries a hash footer (testkey_rsa4096, chained). Its vbmeta
# is built with --flags 3 (verification off) and the bootloader is unlocked, so a footer with --algorithm NONE boots.
AVB_FOOTER := 1

# Download mode: phone off, hold Vol Down + Bixby and plug in USB, then Vol Up to continue.
# One heimdall session per download-mode boot: a second command fails with "Protocol initialisation failed".
FLASH_CMD = heimdall flash --$(TARGET_PARTITION) $(1) --no-reboot

# Kernel from source (scripts/build-kernel.sh): the exact tree of the LineageOS 23.2 (2026-11-14) kernel,
# github.com/LineageOS/android_kernel_samsung_exynos9820 at a412cfaf86b9 (branch lineage-23.2, 4.14.356).
# Clone it into third_party/kernel-exynos9820. DTB (own partition) and recovery DTBO stay prebuilt.
# Without the source tree, build with KERNEL=prebuilt (no HID gadget, no mass storage).
KERNEL_SRC       := kernel-exynos9820
KERNEL_DEFCONFIG := exynos9820-beyond0lte_defconfig
KERNEL_FRAGMENTS := kernel/phonepanel.cfg
KERNEL_DTB_GLOB  :=
# LineageOS builds this tree with Android clang; host clang 23 adds diagnostics that -Werror turns into errors for
# patterns that are fine here, the same ones as on the Exynos 9810 (socs/exynos9810/quirks.md): -Wunused-but-set-global
# (bcmdhd dhd_debug.c), -Wdefault-const-init-*-unsafe, -Wframe-larger-than. bcmdhd re-enables its warnings after
# KCFLAGS, so -Wno-error=, not -Wno-.
KERNEL_KCFLAGS := -Wno-default-const-init-unsafe -Wno-error=unused-but-set-global -Wno-error=frame-larger-than
