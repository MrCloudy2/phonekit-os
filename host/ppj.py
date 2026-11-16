#!/usr/bin/env python3
"""ppj — tiny client for the phonepanel PPJ/1 protocol (protocol/PPJ1.md). Stdlib only.

    from ppj import Panel
    p = Panel()                       # 10.42.0.2:7700 (PANEL mode over USB)
    p.layout([{"id": "cpu", "type": "gauge", "x": 0, "y": 0, "w": 6, "h": 2, "label": "CPU", "unit": "%"}])
    p.set("cpu", value=42)
    for ev in p.events(timeout=0.1): print(ev)
"""
import json
import select
import socket


class Panel:
    def __init__(self, host="10.42.0.2", port=7700, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj, separators=(",", ":")).encode() + b"\n")

    def _lines(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if r:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("phone closed the connection")
            self.buf += chunk
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            if line.strip():
                yield json.loads(line)

    def request(self, obj, want, timeout=3.0):
        """Send and wait for a reply with op == want (other messages are returned in .pending)."""
        self.send(obj)
        import time
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for m in self._lines(max(0, end - time.monotonic())):
                if m.get("op") in (want, "error"):
                    return m
        raise TimeoutError(f"no {want} reply")

    def info(self):
        return self.request({"op": "info"}, "info")

    def layout(self, widgets, page=0, title=""):
        return self.request({"op": "layout", "page": page, "title": title, "widgets": widgets}, "ok")

    def set(self, wid, **fields):
        self.send(dict(op="set", id=wid, **fields))

    def page(self, n):
        return self.request({"op": "page", "n": n}, "ok")

    def events(self, timeout=0.0):
        """Yields phone -> host events (button presses, toggles, sliders) arriving within timeout
        seconds (0: only what is already buffered or waiting on the socket)."""
        import time
        end = time.monotonic() + timeout
        while True:
            for m in self._lines(max(0.0, end - time.monotonic())):
                if m.get("op") == "event":
                    yield m
            if time.monotonic() >= end:
                return
