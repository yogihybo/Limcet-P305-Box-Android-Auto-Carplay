#!/usr/bin/env python3
"""
build_tools/generate_boot_status_logos.py — Generate boot-status bootlogo variants

sd_bootable/bootlogo.raw is the real, hand-placed Toyota logo (never
written by this script) built from make_touch2_bootlogo.py + convert_bootlogo.py
-- confirmed byte-for-byte identical by regenerating it from that same
pipeline. This script reuses make_touch2_bootlogo.py's exact composition
(gradient, cropped/alpha-blended Toyota emblem, wordmark, status-line font/
size/color/position) with only the status string changed, so the other two
boot-progress variants match it pixel-for-pixel in style:

    bootlogo_usb.raw    "Loading USB"     (shown just before attempting bootusb)
    bootlogo_nand.raw   "Loading NAND"    (shown just before falling back to nandboot)

Usage:
  python3 build_tools/generate_boot_status_logos.py
"""

import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
EMBLEM_PATH = "firmware_dumps/Prado firmware dump/mtd6_rootfs/msnprofile/bootlogo/logo17.jpg"
OUT_DIR = Path("sd_bootable")

BG_TOP = (14, 16, 20)
BG_BOTTOM = (30, 33, 40)
TEXT_WHITE = (235, 236, 238)

VARIANTS = [
    ("bootlogo_usb.raw", "Loading USB"),
    ("bootlogo_nand.raw", "Loading NAND"),
    ("bootlogo_sd.raw", "Booting SD Card"),
]


def render(status: str) -> Image.Image:
    """Identical to make_touch2_bootlogo.py's composition, status text parametrized."""
    img = Image.new("RGB", (W, H), BG_TOP)
    draw = ImageDraw.Draw(img)

    for y in range(H):
        t = y / (H - 1)
        r = int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t)
        g = int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t)
        b = int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t)
        draw.line([(0, y), (W, y)], fill=(r, g, b))

    emblem_src = Image.open(EMBLEM_PATH).convert("RGB")
    gray = emblem_src.convert("L")
    bbox = gray.point(lambda p: 255 if p > 12 else 0).getbbox()
    emblem = emblem_src.crop(bbox)
    alpha = gray.crop(bbox).point(lambda p: min(255, int(p * 1.6)))

    emblem_h = 170
    emblem_w = int(emblem.width * emblem_h / emblem.height)
    emblem = emblem.resize((emblem_w, emblem_h), Image.LANCZOS)
    alpha = alpha.resize((emblem_w, emblem_h), Image.LANCZOS)
    emblem_y = int(H * 0.16)
    img.paste(emblem, ((W - emblem_w) // 2, emblem_y), alpha)

    word_font = ImageFont.truetype(FONT_BOLD, 72)
    wordmark = "TOYOTA"
    wbbox = draw.textbbox((0, 0), wordmark, font=word_font)
    tw, th = wbbox[2] - wbbox[0], wbbox[3] - wbbox[1]
    wx = (W - tw) / 2 - wbbox[0]
    wy = emblem_y + emblem_h + 24 - wbbox[1]
    draw.text((wx, wy), wordmark, font=word_font, fill=TEXT_WHITE)

    status_font = ImageFont.truetype(FONT_REG, 28)
    stbbox = draw.textbbox((0, 0), status, font=status_font)
    sttw, stth = stbbox[2] - stbbox[0], stbbox[3] - stbbox[1]
    draw.text(
        ((W - sttw) / 2 - stbbox[0], wy + th + 60 - stbbox[1]),
        status,
        font=status_font,
        fill=TEXT_WHITE,
    )

    return img


def pack_raw(img: Image.Image) -> bytes:
    pixels = img.load()
    out = bytearray(W * H * 4)
    pos = 0
    for y in range(H):
        for x in range(W):
            r, g, b = pixels[x, y]
            struct.pack_into("<I", out, pos, (0xFF << 24) | (r << 16) | (g << 8) | b)
            pos += 4
    return bytes(out)


def main():
    base = OUT_DIR / "bootlogo.raw"
    if not base.exists():
        sys.exit(f"ERROR: {base} not found (this script never writes it, only reads for reference)")

    for filename, status in VARIANTS:
        img = render(status)
        raw = pack_raw(img)
        out_path = OUT_DIR / filename
        out_path.write_bytes(raw)
        print(f"Wrote {out_path}: {len(raw)} bytes ({W}x{H}x32bpp) -- \"{status}\"")

    print(f"{base} left untouched.")


if __name__ == "__main__":
    main()
