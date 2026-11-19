# pptest report: samsung-star2lte

- Date: 2026-11-19T15:46
- PhoneKit: `8618a3e-dirty`
- Result: **ALL PASS**

| Suite | Pass | Fail | Time |
|---|---|---|---|
| display | 5 | 0 | 43 s |
| touch | 10 | 0 | 8 s |
| power | 12 | 0 | 60 s |
| keys | 8 | 0 | 15 s |
| usb | 11 | 0 | 37 s |
| input-modes | 16 | 0 | 26 s |
| panel | 21 | 0 | 169 s |
| phonebox | 9 | 0 | 15 s |

## display
```
PASS  about 1 frame per second  frames in 10.5 s = 11
PASS  damage = clock rectangle only  last damage x,y,w,h = [40, 1880, 576, 64] (0 % of screen)
PASS  zero frames when nothing changes  frames in 10 s = 0
PASS  paneld CPU < 1 % of one core  0.100 %
PASS  CPUs mostly in deepest idle (cluster sleep)  99.1 % of CPU time
```

## touch
```
PASS  10 simultaneous contacts tracked  touches=10
PASS  frames flushed (batched, ≤1 per vsync)  frames=15
PASS  rotation   0: logical 288,422  panel (288, 422) want (288, 422), logical (288, 422)
PASS  rotation  90: logical 592,205  panel (1234, 592) want (1234, 592), logical (592, 205)
PASS  rotation 180: logical 288,422  panel (1151, 2537) want (1151, 2537), logical (288, 422)
PASS  rotation 270: logical 592,205  panel (205, 2367) want (205, 2367), logical (592, 205)
PASS  software latency avg < 34 ms (≤ 2 frames)  min/avg/max us = 13456/27461/44984
PASS  stroke drawn at the touched pixels  rgb(80, 220, 255)
PASS  untouched area unchanged  rgb(16, 16, 24)
PASS  glove_mode command accepted  glove_mode,1:OK | glove_mode,0:OK
```

## power
```
PASS  idle -> dim  state=dim backlight=12
PASS  touch wakes from dim  state=active backlight=128 (configured 128)
PASS  idle -> off (blanked)  state=off
PASS  CPUs capped while off  big-cluster max = 650000 (cpuinfo min 650000)
PASS  host wake < 300 ms (unblank)  unblank took 158 ms (round trip 234 ms)
PASS  CPU caps lifted on wake  big-cluster max = 1794000 (cpuinfo max 2314000, before screen-off 1794000)
PASS  above window -> charging off  battery 55 %, window 45-50, current -210937 uA, status Discharging
PASS  below window -> charging on  current 332500 uA
PASS  over temp_limit -> charging off  battery 30.9 C, limit 28.9 C
PASS  over temp_crit -> screen forced off  state=off
PASS  cooled -> recovers  state=active charging_allowed=1
PASS  off draws < 1/5 of active  788 / 466 / 144 mW
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
PASS  re-enumerated without a phone reboot; ACMs still at 0/2/4  control back after 1.0 s
PASS  HID interface bound (usbhid)  {3: ('0a', 'cdc_acm'), 1: ('0a', 'cdc_acm'), 8: ('03', 'usbhid'), 6: ('03', 'usbhid'), 4: ('02', 'cdc_acm'), 2: ('02', 'cdc_acm'), 0: ('02', 'cdc_acm'), 9: ('03', 'usbhid'), 10: ('ff', 'gud'), 7: ('03', 'usbhid'), 5: ('0a', 'cdc_acm')}
PASS  host sees a phonepanel touchscreen input device  /dev/input/event10
PASS  phone touch arrives on the host at the right position  x=16394 y=16389 (expect ~16383), attempt 1
PASS  control channel recovers after every replug  5/5 ok, worst 1.4 s
PASS  mode B again
PASS  store survives reboot  # /dev/sda11 slot 1 seq 104
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
PASS  info reports grid + size  {'op': 'info', 'version': 'PPJ/1 8618a3e-dirty', 'cols': 6, 'rows': 12, 'w': 1440, 'h': 2960, 'rotation': 0, 'page': 0, 'pages': 0}
PASS  ping → pong  0.4 ms
PASS  unknown op → error
PASS  set on unknown id → error
PASS  layout accepted (8 widgets)  {'op': 'ok', 'page': 0, 'widgets': 8}
PASS  gauge at 100% filled with its #ff0000 accent  rgb(255, 0, 0)
PASS  gauge redraws empty after set 0  rgb(45, 50, 66)
PASS  10 button taps → 10 press events  10/10
PASS  toggle flips 1,0,1,0  [1, 0, 1, 0]
PASS  slider drag → several changes, ends near max / min  [(7, 94.4684), (7, 4.45402)]
PASS  layout on page 1
PASS  page 1 shown
PASS  info counts 2 pages
PASS  page out of range → error  {'op': 'error', 'msg': 'page out of range'}
PASS  60 widgets → capped (≤48) or refused, never crashes  {'op': 'ok', 'page': 2, 'widgets': 48}
PASS  second client answers
PASS  first client was disconnected
PASS  after fuzz: still answers ping
PASS  after fuzz: valid layout still works
PASS  after fuzz: paneld did not restart  uptime 41467 → 210413 ms
```

## phonebox
```
PASS  host adb lists PhoneKit as a device  device usb:1-4 product:phonekit model:PhoneKit device:phonekit transport_id:104
PASS  adb shell works (getprop ro.phonekit = 1)  '1'
PASS  agent set up adb reverse tcp:8765  (is the phonebox agent running?)
PASS  native client connected to the agent's live stream  {'route': 'usb', 'paired': 'yes', 'standby': 'off hold', 'connected': '1', 'config_version': '639305300876679556', 'configs': '1', 'states': '170', 'posts': '0', 'pages': '7', 'page': '0', 'weather': '1', 'tz_off': '7200', 'art': '-1', 'hits': '3'}
PASS  live state arriving (~1/s)  170 → 173
PASS  phonebox pages built  7 pages
PASS  landscape: 12x6 grid
PASS  media play/pause button on the Home page  3 tap targets
PASS  two taps → two POSTs to the agent
```

## Phone info
```
version 8618a3e-dirty
device samsung-star2lte
uptime_ms 33539
paneld_start_ms 3219
idle_reboot_s 0
fb 1440x2960 bpp 32 stride 5760 virt 1440x5920
fb_rgb_offsets 0 8 16
rotation 0 logical 1440x2960
input event5 gpio_keys power
input event0 sec_touchscreen touch power
keys power volup voldown bixby
battery_capacity 55
battery_status Charging
battery_temp 310
battery_voltage_now 3921000
battery_current_now 227
```
