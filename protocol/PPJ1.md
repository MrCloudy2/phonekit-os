# PPJ/1 — phonepanel smart-display protocol, version 1

PANEL mode: the host (PC agent, SBC, microcontroller) sends a **layout** of widgets and then **value updates**;
the phone renders natively and reports touches on interactive widgets as **events**.

## Transport
- **TCP**: `10.42.0.2:7700` over the USB network in PANEL mode (the host gets 10.42.0.1 by DHCP). One client at a
  time; a new connection replaces the old one.
- **Control channel** (debug): `ppctl panel '<json>'`.
- Planned: CDC-ACM for microcontrollers with a USB host port (ESP32-S3, RP2040), Wi-Fi (M10/M11).

## Framing
One JSON object per line (`\n`-terminated, max 16 KiB). UTF-8; only ASCII is rendered by the built-in font.
Unknown fields are ignored; unknown `op`s get an `error` reply. Numbers may be integers or decimals.

## Coordinates
Grid of **cols × rows** cells (portrait 6×12, landscape 12×6; `info` reports the current one). A widget's
`x, y, w, h` are in cells. Rotation re-flows nothing: hosts should re-send the layout after an `info` change event.

## Host → phone
| op | fields | meaning |
|---|---|---|
| `info` | – | phone replies `info` |
| `layout` | `page` (0–7, default 0), `title`, `widgets: [ … ]` | replace one page's widgets |
| `set` | `id`, plus `value` / `text` / `push` / `color` | update a widget (only it is redrawn) |
| `page` | `n` | show page n |
| `clear` | `page` (optional; all if missing) | remove widgets |
| `ping` | – | phone replies `pong` |

### Widget object
`{"id": "cpu", "type": "gauge", "x": 0, "y": 0, "w": 6, "h": 2, …}` — `id` max 23 chars, max 48 widgets per page.

| type | fields | renders |
|---|---|---|
| `label` | `text`, `size` (1–4) | static text |
| `value` | `label`, `value`, `unit`, `decimals` | big number with a caption |
| `gauge` | `label`, `value`, `min`, `max`, `unit` | horizontal bar + number |
| `graph` | `label`, `min`, `max` (auto if omitted); `set` with `push` appends a sample (60 kept) | sparkline |
| `button` | `text` | tap → `event` `press` |
| `toggle` | `text`, `value` (0/1) | tap flips it → `event` `change` |
| `slider` | `label`, `value`, `min`, `max` | drag → `event` `change` (rate-limited) |
| `clock` | `format` (`hms` default, `hm`) | local wall clock (UTC until timezone support) |

Colors: `color` = `"#rrggbb"` (accent) on any widget.

## Phone → host
| op | fields |
|---|---|
| `info` | `version`, `cols`, `rows`, `w`, `h` (logical px), `rotation`, `page`, `pages` |
| `event` | `id`, `type` (`press`/`change`), `value` (toggle 0/1, slider number) |
| `pong` | – |
| `error` | `msg` |

## Example
```
→ {"op":"layout","page":0,"title":"DESK","widgets":[
     {"id":"cpu","type":"gauge","x":0,"y":0,"w":6,"h":2,"label":"CPU","unit":"%","min":0,"max":100},
     {"id":"net","type":"graph","x":0,"y":2,"w":6,"h":3,"label":"NET MB/S"},
     {"id":"mute","type":"toggle","x":0,"y":5,"w":3,"h":2,"text":"MUTE"},
     {"id":"lock","type":"button","x":3,"y":5,"w":3,"h":2,"text":"LOCK"}]}
→ {"op":"set","id":"cpu","value":37}
→ {"op":"set","id":"net","push":4.2}
← {"op":"event","id":"lock","type":"press"}
```
