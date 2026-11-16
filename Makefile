# phonepanel build
#   make device=samsung-xcoverpro            build out/<device>/phonepanel.img
#   make import device=samsung-xcoverpro     import prebuilt kernel/dtb from PREBUILT_FROM (or IMG=...)
#   make flash device=samsung-xcoverpro      flash with heimdall (phone in download mode)
#   make test                                run paneld under qemu-aarch64 (smoke test)
#   BUSYBOX=/path/to/busybox                 optional static aarch64 busybox for a debug shell

device ?= samsung-xcoverpro
DEVDIR := devices/$(device)
ifeq ($(wildcard $(DEVDIR)/device.mk),)
$(error unknown device '$(device)': no $(DEVDIR)/device.mk)
endif
include $(DEVDIR)/device.mk
# personal overrides (e.g. PREBUILT_FROM := /your/recovery.img), never committed
-include $(DEVDIR)/local.mk

OUT     := out/$(device)
VERSION := $(shell git describe --always --dirty 2>/dev/null || echo dev)
CFLAGS  ?= -Os -Wall -Wextra -Wno-unused-result
PP_AUTOREBOOT_SECS ?= 0
# bundle busybox (debug shell) when it has been built: scripts/fetch.sh busybox && scripts/build-busybox.sh
BUSYBOX ?= $(wildcard $(CURDIR)/out/busybox/busybox)

IMG := $(OUT)/phonepanel.img

.PHONY: all import flash deploy logs test clean FORCE
all: $(IMG)

import:
	scripts/import-prebuilt.sh $(device) $(or $(IMG_SRC),$(PREBUILT_FROM))

PANELD_SRC := $(wildcard paneld/*.c)
$(OUT)/paneld: $(PANELD_SRC) paneld/paneld.h FORCE | $(OUT)
	$(CROSS_CC) $(CFLAGS) -D_GNU_SOURCE -static -s -DPP_VERSION='"$(VERSION)"' \
	    -DPP_STORE_PART='"$(TARGET_PARTITION_NAME)"' -DPP_FB_SWAP_RB=$(or $(FB_SWAP_RB),0) \
	    -DPP_AUTOREBOOT_SECS=$(PP_AUTOREBOOT_SECS) -o $@.tmp $(PANELD_SRC)
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(OUT)/version: FORCE | $(OUT)
	@echo "phonepanel $(VERSION) device=$(device)" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(OUT)/ramdisk.cpio.gz: initramfs/initramfs.spec scripts/mkcpio.py $(OUT)/paneld $(OUT)/version | $(OUT)
	PANELD_BIN=$(abspath $(OUT)/paneld) VERSION_FILE=$(abspath $(OUT)/version) \
	BUSYBOX=$(BUSYBOX) python3 scripts/mkcpio.py $< $(OUT)/ramdisk.cpio
	gzip -9 -n -f $(OUT)/ramdisk.cpio

# KERNEL=source (default, scripts/build-kernel.sh) or KERNEL=prebuilt (imported from a known-good image)
KERNEL ?= source
ifeq ($(KERNEL),source)
KERNEL_IMAGE := $(OUT)/kernel/Image
$(KERNEL_IMAGE):
	scripts/build-kernel.sh $(device)
kernel: FORCE
	scripts/build-kernel.sh $(device)
endif

$(IMG): $(OUT)/ramdisk.cpio.gz scripts/mkimage.sh $(DEVDIR)/device.mk $(DEVDIR)/prebuilt/mkbootimg.args $(KERNEL_IMAGE) FORCE
	KERNEL_IMAGE=$(KERNEL_IMAGE) scripts/mkimage.sh $(device) $< $@
	@cd $(OUT) && sha256sum phonepanel.img > phonepanel.img.sha256

$(DEVDIR)/prebuilt/mkbootimg.args:
	@echo "No prebuilt kernel for $(device). Run: make import device=$(device)" >&2; exit 1

flash: $(IMG)
	$(call FLASH_CMD,$(IMG))

# Hands-free loop from a running userdebug ROM (no download mode, no buttons):
#   make deploy   build, dd onto the phonepanel partition over adb, verify, reboot into it
#   make logs     after it rebooted back into the ROM: phonepanel lines from last_kmsg
deploy: $(IMG)
	scripts/phone.sh flash $(IMG) $(TARGET_PARTITION_NAME)
	scripts/phone.sh boot

logs:
	scripts/phone.sh logs

test: $(OUT)/paneld
	qemu-aarch64-static $(OUT)/paneld & pid=$$!; sleep 2; kill $$pid 2>/dev/null; wait $$pid 2>/dev/null; true

$(OUT):
	mkdir -p $@

clean:
	rm -rf out
