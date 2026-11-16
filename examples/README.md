# Examples: things you can turn the phone into

PhoneKit is a base, not a product. These are starting points; fork and bend them.

| Example | Mode | Status | Where |
|---|---|---|---|
| PC stats dashboard (CPU, RAM, network, buttons, toggle, slider) | PANEL | 🟨 protocol in progress | [`host/demo_dashboard.py`](../host/demo_dashboard.py) |
| phonebox desktop dashboards on the phone, natively | PANEL | ⬜ planned (M12b) | — |
| Second USB monitor with touch | SCREEN | ✅ | `ppctl.py mode 0` or the side-button menu; Linux `gud` |
| Trackpad / keyboard / both for a headless box or a TV PC | MOUSE / KEYBOARD / COMBO | ✅ | side buttons → menu |
| Macro pad: buttons that run host commands | PANEL | ⬜ | write it with [`host/ppj.py`](../host/ppj.py): `events()` → `subprocess` |
| Home Assistant wall panel | PANEL | ⬜ idea | an HA automation or small script that speaks [PPJ/1](../protocol/PPJ1.md) |
| Microcontroller display (ESP32-S3/RP2040 as USB host) | PANEL over serial | ⬜ planned | protocol is JSON lines, easy on an MCU |
| Portable debug device: flash drive + serial console + shell | FLASH DRIVE | ⬜ planned (M6d) | — |
| Something with no host at all: a clock, a timer, a status light | standalone | ⬜ (M9) | paneld C code (`paneld/ui.c`, `paneld/panel.c`) |

## Writing your own host program
PANEL mode is one JSON object per line over TCP `10.42.0.2:7700` (USB network). The smallest possible client:

```python
import socket, json, time
s = socket.create_connection(("10.42.0.2", 7700))
send = lambda o: s.sendall((json.dumps(o) + "\n").encode())
send({"op": "layout", "widgets": [{"id": "t", "type": "value", "x": 0, "y": 0, "w": 6, "h": 3, "label": "HELLO"}]})
for i in range(100):
    send({"op": "set", "id": "t", "value": i}); time.sleep(1)
```

Full spec: [protocol/PPJ1.md](../protocol/PPJ1.md). PRs with new examples are welcome: put them in
`examples/<name>/` with a short README.
