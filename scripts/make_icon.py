#!/usr/bin/env python3
"""
Write a 256x256 PNG icon for a packaged game.

    ./scripts/make_icon.py out.png demo

appimagetool will not build without an icon and the .desktop entry needs one, so a package
cannot be produced at all until one exists. Generating a placeholder keeps that from being
a dependency on an artist, and keeps `scripts/build_release.sh` from failing on a machine where
nobody has drawn anything yet. A game that ships its own art puts it at
`game/<name>/icon.png` and this is never called.

Written by hand rather than through Pillow or ImageMagick: neither is otherwise a
dependency of this repository, and a PNG with one filter type and a zlib stream is about
thirty lines. The alternative is asking every machine that builds a release to install an
image library to draw a rounded square.
"""

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

SIZE = 256


def chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def colours(name):
    """Two related hues, derived from the name so different games look different."""
    digest = hashlib.sha256(name.encode("utf-8")).digest()
    base = (digest[0], digest[1], digest[2])
    # Lift toward white for the foreground so the glyph stays legible whatever the hash
    # produced, including a background that came out nearly black.
    fg = tuple(min(255, c // 2 + 150) for c in base)
    bg = tuple(c // 3 + 20 for c in base)
    return bg, fg


def render(name):
    """RGBA rows: a rounded square in the background colour, with a diagonal slab."""
    bg, fg = colours(name)
    radius = SIZE // 6
    rows = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            # Rounded-corner test: only the four corner boxes need a distance check.
            cx = radius - x if x < radius else (x - (SIZE - 1 - radius) if x > SIZE - 1 - radius else 0)
            cy = radius - y if y < radius else (y - (SIZE - 1 - radius) if y > SIZE - 1 - radius else 0)
            if cx * cx + cy * cy > radius * radius:
                row += bytes((0, 0, 0, 0))
                continue

            # A thick diagonal band, which reads as a mark at 32px and needs no font.
            d = (x + y) - SIZE
            row += bytes(fg + (255,)) if -SIZE // 8 < d < SIZE // 8 else bytes(bg + (255,))
        rows.append(bytes(row))
    return rows


def write_png(path, rows):
    raw = b"".join(b"\x00" + row for row in rows)  # filter type 0 per scanline
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))
    path.write_bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("name")
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_png(args.out, render(args.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
