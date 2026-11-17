# Research: Bluetooth (M10c) on the XCover Pro

Status 2026-11-17: **feasible, not started; a clear path below.**

## What's there
- Samsung's SCSC Bluetooth driver is built in (`CONFIG_SCSC_BT=y`), sharing the chip and firmware with Wi-Fi
  (`/vendor/etc/wifi/mx140_bt.hcf` is already in PhoneKit's image). It exposes **`/dev/scsc_h4_0`**: a raw H4 HCI
  byte stream, which Android's userspace stack (Bluedroid/Fluoride) talks to directly.
- The Linux Bluetooth core (`CONFIG_BT`, BlueZ's kernel side) is **not** compiled in.

## Path
1. Kernel fragment: `CONFIG_BT=y`, `CONFIG_BT_HCIVHCI=y` (virtual HCI from userspace), `CONFIG_BT_HIDP=y`,
   `CONFIG_BT_LE=y`; rebuild (`make KERNEL=source`).
2. A tiny bridge in paneld: copy H4 packets between `/dev/scsc_h4_0` and `/dev/vhci` → `hci0` appears.
   (Check whether the chip needs an HCI reset / vendor init first: trace Bluedroid's first packets on the ROM.)
3. The killer feature: **phone as a Bluetooth keyboard/trackpad** (HID device role) for MOUSE/KEYBOARD/COMBO
   without a cable. That needs an SDP record + L2CAP control/interrupt channels (PSM 17/19); doable with raw
   sockets and a minimal SDP server, without bluetoothd/D-Bus.
4. BLE peripheral (GATT) for microcontroller projects is a smaller second step.

NFC stays parked: it's tied to the secure element (disabling it hung boot in M1) and needs an NCI stack port.
