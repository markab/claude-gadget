#!/usr/bin/env python3
"""Capture device screenshots over USB serial.

Sends 's' to the gadget, which renders every page with demo data and streams
each as raw RGB565. Saves round (transparent-cornered) PNGs to docs/images/.

    ~/.platformio/penv/bin/python tools/screenshots.py [/dev/cu.usbmodemXXXX]

Needs only pyserial (bundled with PlatformIO's Python).
"""
import glob
import os
import struct
import sys
import time
import zlib

import serial

OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "images")


def write_png(path, w, h, rgb565):
    r2 = (w / 2) ** 2
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")  # filter: none
        for x in range(w):
            i = (y * w + x) * 2
            v = rgb565[i] | (rgb565[i + 1] << 8)
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            dx, dy = x - w / 2 + 0.5, y - h / 2 + 0.5
            a = 255 if dx * dx + dy * dy <= r2 else 0  # round display
            row += bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6, a))
        rows.append(bytes(row))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


EXPECTED = ["clock", "usage", "settings", "sound", "wifi", "battery", "device", "setup-hotspot"]


def capture_round(s, saved):
    """One 's' request. Saves every image whose trailer checks out; a log line
    landing mid-transfer shifts the bytes (a torn image), so those are skipped."""
    s.reset_input_buffer()
    s.write(b"s")
    deadline = time.time() + 120
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line == "SNAPDONE":
            return
        if not line.startswith("SNAP "):
            continue  # regular log output
        _, name, w, h = line.split()
        w, h = int(w), int(h)
        data = s.read(w * h * 2)
        tail = s.read(5)  # "\nEND\n"
        if len(data) != w * h * 2 or tail != b"\nEND\n":
            print(f"  {name}: corrupted in transfer, will retry")
            continue
        if name in saved:
            continue
        path = os.path.join(OUT, f"{name}.png")
        write_png(path, w, h, data)
        saved.add(name)
        print(f"{name}: {w}x{h} -> {os.path.relpath(path)}")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else (glob.glob("/dev/cu.usbmodem*") or [None])[0]
    if not port:
        sys.exit("No /dev/cu.usbmodem* port found; pass one explicitly.")
    os.makedirs(OUT, exist_ok=True)
    s = serial.Serial(port, 115200, timeout=10)
    saved = set()
    for attempt in range(5):
        capture_round(s, saved)
        missing = [n for n in EXPECTED if n not in saved]
        if not missing:
            print("done")
            return
        print(f"retrying: {', '.join(missing)}")
    sys.exit(f"gave up on: {', '.join(missing)}")


if __name__ == "__main__":
    main()
