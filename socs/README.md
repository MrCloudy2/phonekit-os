# SoC notes

Phones built on the same chip share most of the hard parts: the display controller driver, USB controller,
sensor hub and kernel tree quirks. Write those down here once, so every port on that SoC starts from them.

```
socs/<soc>/
  quirks.md        what bit you, why, and the fix (and in which device.mk variable/define it lives)
  kernel.cfg       optional: config fragment lines that are safe or required on every phone with this SoC
```

Naming: lowercase vendor-less chip name as the kernel calls it (`exynos9611`, `sdm660`, `sm7125`, `mt6765`).
Link each device that uses it from `quirks.md`, and set `identity.soc_dir` in the device's `device.yaml`.

| SoC | Devices | Notes |
|---|---|---|
| [exynos8895](exynos8895/quirks.md) | [samsung-dreamlte](../devices/samsung-dreamlte/device.yaml) (in progress) | Samsung header v0 + DT boot image, DECON fbdev, DWC3, SSP sensor hub |
| [exynos9611](exynos9611/quirks.md) | [samsung-xcoverpro](../devices/samsung-xcoverpro/device.yaml) | Samsung DECON fbdev, DWC3, CHUB sensor hub |
| [exynos9810](exynos9810/quirks.md) | [samsung-star2lte](../devices/samsung-star2lte/device.yaml) (in progress) | Samsung header v0 + DT boot image (like the 8895), DECON fbdev, DWC3 composite, SSP on the GPS chip |
| [exynos9820](exynos9820/quirks.md) | [samsung-beyond0lte](../devices/samsung-beyond0lte/device.yaml) (in progress) | AOSP header v1 + recovery DTBO + AVB, DECON fbdev, DWC3 via UDC (composite without old configfs), SSP on the GPS chip |
