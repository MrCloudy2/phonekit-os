# pptest report: samsung-beyond0lte

- Date: 2026-11-19T17:34
- PhoneKit: `12b2c94-dirty`
- Result: **1 FAILED**

| Suite | Pass | Fail | Time |
|---|---|---|---|
| display | 5 | 0 | 44 s |
| touch | 10 | 0 | 7 s |
| power | 11 | 1 | 60 s |
| keys | 8 | 0 | 15 s |
| usb | 11 | 0 | 83 s |
| input-modes | 16 | 0 | 25 s |
| panel | 21 | 0 | 167 s |
| phonebox | 9 | 0 | 14 s |

## display
```
PASS  about 1 frame per second  frames in 10.5 s = 12
PASS  damage = clock rectangle only  last damage x,y,w,h = [40, 1455, 576, 112] (2 % of screen)
PASS  zero frames when nothing changes  frames in 10 s = 0
PASS  paneld CPU < 1 % of one core  0.050 %
PASS  CPUs mostly in deepest idle (cluster sleep)  99.9 % of CPU time
```

## touch
```
PASS  10 simultaneous contacts tracked  touches=10
PASS  frames flushed (batched, ≤1 per vsync)  frames=13
PASS  rotation   0: logical 216,325  panel (216, 325) want (216, 325), logical (216, 325)
PASS  rotation  90: logical 456,154  panel (925, 456) want (925, 456), logical (456, 154)
PASS  rotation 180: logical 216,325  panel (863, 1954) want (863, 1954), logical (216, 325)
PASS  rotation 270: logical 456,154  panel (154, 1823) want (154, 1823), logical (456, 154)
PASS  software latency avg < 34 ms (≤ 2 frames)  min/avg/max us = 12052/27807/28969
PASS  stroke drawn at the touched pixels  rgb(80, 220, 255)
PASS  untouched area unchanged  rgb(16, 16, 24)
PASS  glove_mode command accepted  glove_mode,1:OK | glove_mode,0:OK
```

## power
```
PASS  idle -> dim  state=dim backlight=12
PASS  touch wakes from dim  state=active backlight=128 (configured 128)
PASS  idle -> off (blanked)  state=off
PASS  CPUs capped while off  big-cluster max = 507000 (cpuinfo min 507000)
PASS  host wake < 300 ms (unblank)  unblank took 198 ms (round trip 274 ms)
PASS  CPU caps lifted on wake  big-cluster max = 2314000 (cpuinfo max 2314000, before screen-off 2314000)
PASS  above window -> charging off  battery 82 %, window 72-77, current 0 uA, status Discharging
PASS  below window -> charging on  current 151562 uA
PASS  over temp_limit -> charging off  battery 30.7 C, limit 28.7 C
PASS  over temp_crit -> screen forced off  state=off
PASS  cooled -> recovers  state=active charging_allowed=1
FAIL  off draws < 1/5 of active  712 / 407 / 143 mW
```

## keys
```
SKIP  top key -> pages  no top key on this phone
PASS  vol down long -> next page  page 1 -> 0
PASS  vol up long -> previous page  page 0 -> 1 (start 1)
PASS  volume up/down step the backlight  128 -> up 170 -> down x2 90
PASS  bixby long -> mode menu
PASS  bixby long again -> menu closed
PASS  power short -> screen off
PASS  first key in the dark only wakes  state active, bright 128 -> 128
PASS  key.volup.short=page_next takes effect  page 1 -> 0
```

## usb
```
PASS  3 ACM ports at interfaces 0/2/4  {3: ('0a', 'cdc_acm'), 1: ('0a', 'cdc_acm'), 8: ('ff', 'usbfs'), 6: ('e0', 'rndis_host'), 4: ('02', 'cdc_acm'), 2: ('02', 'cdc_acm'), 0: ('02', 'cdc_acm'), 7: ('0a', 'rndis_host'), 5: ('0a', 'cdc_acm')}
PASS  network function bound on the host  {3: ('0a', 'cdc_acm'), 1: ('0a', 'cdc_acm'), 8: ('ff', 'usbfs'), 6: ('e0', 'rndis_host'), 4: ('02', 'cdc_acm'), 2: ('02', 'cdc_acm'), 0: ('02', 'cdc_acm'), 7: ('0a', 'rndis_host'), 5: ('0a', 'cdc_acm')}
PASS  host gets DHCP and reaches the phone (ping 10.42.0.2)  host interface enp0s20f0u4i6
PASS  re-enumerated without a phone reboot; ACMs still at 0/2/4  control back after 1.6 s
PASS  HID interface bound (usbhid)  {3: ('0a', 'cdc_acm'), 1: ('0a', 'cdc_acm'), 8: ('03', 'usbhid'), 6: ('03', 'usbhid'), 4: ('02', 'cdc_acm'), 2: ('02', 'cdc_acm'), 0: ('02', 'cdc_acm'), 9: ('03', 'usbhid'), 10: ('ff', 'gud'), 7: ('03', 'usbhid'), 5: ('0a', 'cdc_acm')}
PASS  host sees a phonepanel touchscreen input device  /dev/input/event10
PASS  phone touch arrives on the host at the right position  x=16398 y=16390 (expect ~16383), attempt 1
PASS  control channel recovers after every replug  5/5 ok, worst 1.0 s
PASS  mode B again
PASS  store survives reboot  # /dev/sda15 slot 1 seq 56
PASS  USB mode survives reboot
```

## input-modes
```
PASS  host sees phonepanel mouse + keyboard  mouse=/dev/input/event11 keyboard=/dev/input/event12
PASS  one-finger swipe moves the pointer right/down  dx=1166 dy=1166
PASS  tap = left click  [272]
PASS  two-finger tap = right click  [273]
PASS  three-finger tap = middle click  [274]
PASS  two-finger drag scrolls  wheel=12
PASS  bottom-left zone = left button  [272]
PASS  tap A types KEY_A  [30]
PASS  sticky Shift then A = Shift+A  [42, 30]
PASS  tap in the trackpad strip = click  [272]
PASS  keys still type in combo  [30]
PASS  landscape: tapping A (logical coords) types KEY_A  [30]
PASS  mode next: SCREEN -> MOUSE  input_mode MOUSE
PASS  menu opens
PASS  menu: vol down + xcover selects KEYBOARD  input_mode KEYBOARD menu_open 0 settings_open 0 owner 2 buttons 0 sticky 0 rotation 0
PASS  SCREEN: host picture restored after other modes  identical screenshots
```

## panel
```
PASS  TCP 7700 reachable over the USB network
PASS  info reports grid + size  {'op': 'info', 'version': 'PPJ/1 12b2c94-dirty', 'cols': 6, 'rows': 12, 'w': 1080, 'h': 2280, 'rotation': 0, 'page': 0, 'pages': 0}
PASS  ping → pong  0.5 ms
PASS  unknown op → error
PASS  set on unknown id → error
PASS  layout accepted (8 widgets)  {'op': 'ok', 'page': 0, 'widgets': 8}
PASS  gauge at 100% filled with its #ff0000 accent  rgb(255, 0, 0)
PASS  gauge redraws empty after set 0  rgb(45, 50, 66)
PASS  10 button taps → 10 press events  10/10
PASS  toggle flips 1,0,1,0  [1, 0, 1, 0]
PASS  slider drag → several changes, ends near max / min  [(8, 94.9612), (8, 3.87597)]
PASS  layout on page 1
PASS  page 1 shown
PASS  info counts 2 pages
PASS  page out of range → error  {'op': 'error', 'msg': 'page out of range'}
PASS  60 widgets → capped (≤48) or refused, never crashes  {'op': 'ok', 'page': 2, 'widgets': 48}
PASS  second client answers
PASS  first client was disconnected
PASS  after fuzz: still answers ping
PASS  after fuzz: valid layout still works
PASS  after fuzz: paneld did not restart  uptime 41425 → 207738 ms
```

## phonebox
```
PASS  host adb lists PhoneKit as a device  device usb:1-4 product:phonekit model:PhoneKit device:phonekit transport_id:33
PASS  adb shell works (getprop ro.phonekit = 1)  '1'
PASS  agent set up adb reverse tcp:8765  (is the phonebox agent running?)
PASS  native client connected to the agent's live stream  {'route': 'usb', 'paired': 'yes', 'standby': 'off hold', 'connected': '1', 'config_version': '639305300876679556', 'configs': '1', 'states': '166', 'posts': '0', 'pages': '7', 'page': '0', 'weather': '1', 'tz_off': '7200', 'art': '-1', 'hits': '3'}
PASS  live state arriving (~1/s)  166 → 169
PASS  phonebox pages built  7 pages
PASS  landscape: 12x6 grid
PASS  media play/pause button on the Home page  3 tap targets
PASS  two taps → two POSTs to the agent
```

## Phone info
```
version 12b2c94-dirty
device samsung-beyond0lte
uptime_ms 112463
paneld_start_ms 2109
idle_reboot_s 0
fb 1080x2280 bpp 32 stride 4320 virt 1080x4560
fb_rgb_offsets 0 8 16
rotation 0 logical 1080x2280
input event1 gpio_keys power
input event2 sec_touchscreen touch
keys power volup voldown bixby
battery_capacity 82
battery_status Discharging
battery_temp 309
battery_voltage_now 4118000
battery_current_now 0
```
