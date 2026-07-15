#!/usr/bin/env python3
"""Generate an 800x480 bootlogo styled after the Toyota Touch2 splash screen,
showing the dumped Toyota emblem above "TOYOTA" and "Loading U-Boot" — for
testing the bootlogo.raw display pipeline.

Usage:
  python3 make_touch2_bootlogo.py [output.png]
"""

import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
OUT = sys.argv[1] if len(sys.argv) > 1 else "touch2_bootlogo.png"
EMBLEM_PATH = (
    "firmware_dumps/Prado firmware dump/mtd6_rootfs/msnprofile/bootlogo/logo17.jpg"
)

BG_TOP = (14, 16, 20)
BG_BOTTOM = (30, 33, 40)
TEXT_WHITE = (235, 236, 238)

img = Image.new("RGB", (W, H), BG_TOP)
draw = ImageDraw.Draw(img)

# vertical gradient background, like the Touch2 splash
for y in range(H):
    t = y / (H - 1)
    r = int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t)
    g = int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t)
    b = int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t)
    draw.line([(0, y), (W, y)], fill=(r, g, b))

# dumped Toyota emblem (logo17.jpg is full-frame on black; crop the emblem
# bounding box, then alpha-composite by luminance so the black background
# blends into our gradient instead of showing as a hard box)
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

# wordmark, Toyota-style condensed caps, below the emblem
word_font = ImageFont.truetype(FONT_BOLD, 72)
wordmark = "TOYOTA"
wbbox = draw.textbbox((0, 0), wordmark, font=word_font)
tw, th = wbbox[2] - wbbox[0], wbbox[3] - wbbox[1]
wx = (W - tw) / 2 - wbbox[0]
wy = emblem_y + emblem_h + 24 - wbbox[1]
draw.text((wx, wy), wordmark, font=word_font, fill=TEXT_WHITE)

# status text
status_font = ImageFont.truetype(FONT_REG, 28)
status = "Loading U-Boot"
stbbox = draw.textbbox((0, 0), status, font=status_font)
sttw, stth = stbbox[2] - stbbox[0], stbbox[3] - stbbox[1]
draw.text(
    ((W - sttw) / 2 - stbbox[0], wy + th + 60 - stbbox[1]),
    status,
    font=status_font,
    fill=TEXT_WHITE,
)

img.save(OUT)
print(f"wrote {OUT}")
