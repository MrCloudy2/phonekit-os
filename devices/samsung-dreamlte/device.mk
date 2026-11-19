# Build settings for the Samsung Galaxy S8 (SM-G950F/FD, exynos8895). Port in progress: nothing here has
# booted yet. Human-readable facts and their sources live in device.yaml; how paneld drives the hardware in hw.c.
# Only the Exynos model. The Snapdragon S8 (SM-G950U/U1/W) can't unlock its bootloader.

ARCH            := arm64
CROSS_CC        ?= aarch64-linux-gnu-gcc

# Partition name is upper case on this phone (GPT PARTNAME, heimdall and by-name all say RECOVERY),
# from TWRP 3.7.0's recovery.fstab: /dev/block/platform/11120000.ufs/by-name/RECOVERY
TARGET_PARTITION      := RECOVERY
TARGET_PARTITION_NAME := RECOVERY
# From the PIT (heimdall print-pit, 2026-11-18): 11776 blocks of 4 KiB (UFS). Confirm with blockdev in TWRP.
TARGET_PARTITION_SIZE := 48234496

# Known-good image (TWRP for dreamlte, or the stock recovery.img from the phone's firmware).
# Put your local path in local.mk (gitignored) or pass IMG_SRC=… to make import.
# The image uses Samsung's header v0 + DT layout; import/mkimage handle it via scripts/samsung-bootimg.py.
PREBUILT_FROM ?= /path/to/twrp-3.7.0_9-0-dreamlte.img

# Launched on Android 7 with Samsung's own signing, no AVB: TWRP's image has no footer.
AVB_FOOTER := 0

# Download mode: phone off, hold Vol Down + Bixby + Power.
FLASH_CMD = heimdall flash --$(TARGET_PARTITION) $(1) --no-reboot

# Kernel from source (scripts/build-kernel.sh; `make device=samsung-dreamlte`, KERNEL=source is the default):
# github.com/8890q/android_kernel_samsung_universal8895, branch lineage-21 (4.4.302), the tree behind the /e/OS and
# LineageOS 21 S8 builds. Clone it into third_party/kernel-universal8895. The DT table stays prebuilt: import the
# matching /e/OS recovery.img (`make import device=samsung-dreamlte IMG_SRC=<path>/recovery.img`).
# Without the source tree, build with KERNEL=prebuilt (the imported kernel: no HID gadget).
KERNEL_SRC       := kernel-universal8895
KERNEL_DEFCONFIG := exynos8895-dreamlte_defconfig
KERNEL_FRAGMENTS := kernel/phonepanel.cfg
KERNEL_DTB_GLOB  :=
# The 4.4 Makefile passes clang --prefix=<binutils dir>/ and turns off the integrated assembler; clang 23 then runs
# the host's x86 `as` ("unrecognized option '-EL'"). Give it the full cross prefix instead.
# Its build rules also link empty directories with `$(LD) -r` and no objects, which ld.lld rejects ("target
# emulation unknown"): link with GNU ld.
KERNEL_MAKE_FLAGS := CLANG_PREFIX=--prefix=$(dir $(shell which aarch64-linux-gnu-as))aarch64-linux-gnu- \
                     LD=aarch64-linux-gnu-ld \
                     KCFLAGS=-Wno-strict-prototypes
# (Samsung's code is full of "int f()" declarations, an error under -Werror with clang 23 but harmless. Real type
# errors still stop the build: kernel-patches/ fixes three backport bugs clang 23 found.)
