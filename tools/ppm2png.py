# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# ppm to png: the server frame dumps are plain ppm, the shots travel
# to the user as png. usage: python3 tools/ppm2png.py frame.ppm [out.png]

import struct
import sys
import zlib
from pathlib import Path


def ppm_to_png(src, dst):
    raw = Path(src).read_bytes()
    head, sep, body = raw.partition(b"255\n")
    if not sep:
        raise ValueError(f"{src}: not a maxval 255 ppm")
    lines = head.split(b"\n")
    w, h = map(int, lines[1].split())
    if len(body) < w * h * 3:
        raise ValueError(f"{src}: truncated body")
    body = body[:w * h * 3]
    rows = b"".join(b"\x00" + body[y * w * 3:(y + 1) * w * 3]
                    for y in range(h))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    Path(dst).write_bytes(png)
    return w, h


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for arg in sys.argv[1:]:
        src = Path(arg)
        dst = src.with_suffix(".png")
        w, h = ppm_to_png(src, dst)
        print(f"{src} -> {dst} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
