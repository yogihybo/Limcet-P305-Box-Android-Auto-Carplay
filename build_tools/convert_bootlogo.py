#!/usr/bin/env python3
"""
build_tools/convert_bootlogo.py — Convert the dumped bootlogo JPEG into a raw OSD framebuffer

U-Boot on the ARK1668 has no JPEG decoder available to us (the stock binary's
`jpeghw`/`jpeg decode` commands use a proprietary hardware codec with no
released source — see docs/uboot_build.md). To show the boot logo from U-Boot
without porting that hardware protocol, we pre-decode the JPEG offline and
write a raw 32bpp buffer that U-Boot can `fatload` straight into RAM and hand
to the OSD1 layer via ark_set_osd_addr()/ark_set_osd_image() — the same OSD
primitives already used (on OSD2) for the firmware-update progress overlay in
board/arkmicro/ark1668_limcet_p305/ark1668_display_cfg.c.

Pixel format: the panel is configured DISP_RGB_888 + RGB_MODE_BGR, and the
existing progress-bar code writes 32-bit *(unsigned int *)p = color values
using standard 0xAARRGGBB constants (e.g. 0xff00ff00 for green). Since that
is a plain little-endian 32-bit store, the bytes actually land in memory as
B, G, R, A — so each output pixel is packed as (0xFF<<24)|(R<<16)|(G<<8)|B
and written little-endian, matching that existing convention exactly.

Usage:
  python3 build_tools/convert_bootlogo.py bootlogo.jpg bootlogo.raw
  python3 build_tools/convert_bootlogo.py bootlogo.jpg bootlogo.raw --width 800 --height 480
"""

import argparse
import struct
import sys

from PIL import Image


def convert(src_path: str, dst_path: str, width: int, height: int) -> None:
    img = Image.open(src_path).convert("RGB")
    if img.size != (width, height):
        print(f"WARNING: source is {img.size}, resizing to {width}x{height}")
        img = img.resize((width, height))

    pixels = img.load()
    out = bytearray(width * height * 4)
    pos = 0
    for y in range(height):
        for x in range(width):
            r, g, b = pixels[x, y]
            struct.pack_into("<I", out, pos, (0xFF << 24) | (r << 16) | (g << 8) | b)
            pos += 4

    with open(dst_path, "wb") as f:
        f.write(out)

    print(f"Wrote {dst_path}: {len(out)} bytes ({width}x{height}x32bpp)")
    print("Copy this file to the SD card FAT partition (next to UBOOT.BIN) as bootlogo.raw")


def main():
    parser = argparse.ArgumentParser(description="Convert bootlogo JPEG to raw OSD framebuffer")
    parser.add_argument("input", help="Source JPEG (e.g. mtd8_bootlogo/bootlogo)")
    parser.add_argument("output", help="Output raw file (e.g. bootlogo.raw)")
    parser.add_argument("--width", type=int, default=800)
    parser.add_argument("--height", type=int, default=480)
    args = parser.parse_args()

    try:
        convert(args.input, args.output, args.width, args.height)
    except FileNotFoundError:
        sys.exit(f"ERROR: input file not found: {args.input}")


if __name__ == "__main__":
    main()
