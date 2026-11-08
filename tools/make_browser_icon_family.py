#!/usr/bin/env python3
"""Pad the browser's 22x22 monochrome icon to Palm's 32x32 family size."""

import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "icon-1.bmp"
OUTPUT = ROOT / "assets" / "icon-1-32.bmp"


def main() -> None:
    data = SOURCE.read_bytes()
    if data[:2] != b"BM":
        raise SystemExit("source is not a Windows BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    depth = struct.unpack_from("<H", data, 28)[0]
    if (width, height, depth) != (22, 22, 1):
        raise SystemExit("expected a 22x22x1 source icon")

    source_stride = 4
    rows = [bytearray(4) for _ in range(32)]
    for y in range(22):
        source_row = data[offset + y * source_stride:
                          offset + (y + 1) * source_stride]
        target = rows[y + 5]
        for x in range(22):
            if source_row[x // 8] & (0x80 >> (x & 7)):
                target_x = x + 5
                target[target_x // 8] |= 0x80 >> (target_x & 7)

    palette = data[54:62]
    pixel_bytes = b"".join(rows)
    output_offset = 62
    header = (
        b"BM"
        + struct.pack("<IHHI", output_offset + len(pixel_bytes), 0, 0,
                      output_offset)
        + struct.pack("<IIIHHIIIIII", 40, 32, 32, 1, 1, 0,
                      len(pixel_bytes), 3780, 3780, 2, 2)
    )
    OUTPUT.write_bytes(header + palette + pixel_bytes)


if __name__ == "__main__":
    main()
