# Research: WEBCAM mode (M6b) on the XCover Pro

Status 2026-11-17: **not feasible without a large reverse-engineering effort; parked.** Notes for whoever picks it up.

## What exists under PhoneKit
- **Camera driver**: Samsung's `fimc-is` (FIMC-IS image signal processor) is built into the kernel and registers
  V4L2 nodes: `exynos-fimc-is-ss0/1/2` (sensors), `-30s/30c/30p`, `-31s/31c/31p` (ISP paths), plus
  `s5p-mfc` (hardware video encoder) and `exynos-jpeg`.
- **USB side**: `CONFIG_USB_CONFIGFS_F_UVC` is **not** set; enabling it in `kernel/phonepanel.cfg` would give a
  configfs `uvc` function (needs a userspace UVC gadget daemon to feed frames).

## Why it stops there
- `fimc-is` isn't a standard V4L2 camera: Android's Samsung camera HAL loads the ISP firmware and per-sensor
  "setfiles" from the vendor partition, then drives the pipeline with private ioctls and a fixed open/sensor/
  stream sequence. Plain `v4l2-ctl --stream-mmap` on these nodes doesn't produce frames without that sequence.
- Reproducing the HAL sequence means tracing the vendor HAL (strace + ioctl decoding on the ROM) per sensor.
- postmarketOS has no working camera for Exynos 9611 either (as of this note).

## If someone tries
1. In LineageOS: `strace -f -e ioctl` the camera HAL while an app opens the rear camera; list the firmware and
   setfiles it opens under `/vendor/firmware`.
2. Copy those files (prebuilt/, never committed), replay the open/ioctl sequence from a small C tool under PhoneKit.
3. If frames come out (likely YUV/NV21), add `CONFIG_USB_CONFIGFS_F_UVC=y` and a UVC gadget loop (MJPEG via
   `exynos-jpeg` or uncompressed YUY2 at a low resolution over USB 2.0).
Phones whose cameras work on mainline Linux (some Qualcomm devices via libcamera/postmarketOS) are a much
easier target for this mode.
