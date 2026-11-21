#!/usr/bin/env python3
"""Convert the ASCII PBM previews of tools/rlcd42/render_preview.cpp into PNG files.

Usage:
    python3 tools/rlcd42/pbm_to_png.py <output_dir> [--scale 2]

Every .pbm in <output_dir> becomes a .png next to it. A set pixel is black, an unset
pixel white, matching the reflective panel.
"""
import argparse
import pathlib
import struct
import sys
import zlib


def read_pbm(path: pathlib.Path) -> tuple[int, int, bytearray]:
    """Reads a P1 (ASCII) PBM into a row-major buffer of 1 = black, 0 = white.

    The preview writer emits one bit per character without separators, so the pixel
    area is read character by character and whitespace is ignored.
    """
    text = path.read_text().split()
    if not text or text[0] != "P1":
        raise ValueError(f"{path}: not an ASCII PBM")
    width, height = int(text[1]), int(text[2])
    pixels = "".join(text[3:])
    bits = bytearray(width * height)
    if len(pixels) != width * height:
        raise ValueError(f"{path}: expected {width * height} pixels, got {len(pixels)}")
    for i, value in enumerate(pixels):
        if value == "1":
            bits[i] = 1
        elif value != "0":
            raise ValueError(f"{path}: bad pixel {value!r}")
    return width, height, bits


def write_png(path: pathlib.Path, width: int, height: int, bits: bytearray, scale: int) -> None:
    """Writes an 8-bit grayscale PNG with nearest-neighbour upscaling."""
    raw = bytearray()
    for y in range(height):
        row = bits[y * width:(y + 1) * width]
        expanded = bytearray()
        for value in row:
            expanded += bytes([0 if value else 255]) * scale
        for _ in range(scale):
            raw.append(0)  # PNG filter type 0 (None).
            raw += expanded
    out_w, out_h = width * scale, height * scale

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="directory holding the .pbm previews")
    parser.add_argument("--scale", type=int, default=2, help="pixel upscale factor")
    args = parser.parse_args()

    directory = pathlib.Path(args.directory)
    files = sorted(directory.glob("*.pbm"))
    if not files:
        print(f"no .pbm files in {directory}", file=sys.stderr)
        return 1
    for pbm in files:
        width, height, bits = read_pbm(pbm)
        png = pbm.with_suffix(".png")
        write_png(png, width, height, bits, args.scale)
        print(f"{pbm.name} -> {png.name} ({width * args.scale}x{height * args.scale})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
