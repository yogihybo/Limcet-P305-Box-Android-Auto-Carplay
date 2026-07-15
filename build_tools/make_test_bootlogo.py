#!/usr/bin/env python3
"""Generate an 800x480 test image with "U-boot loading" text, for testing
the bootlogo.raw display pipeline."""

from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
TEXT = "U-boot loading"

img = Image.new("RGB", (W, H), (20, 20, 40))
draw = ImageDraw.Draw(img)

font = ImageFont.truetype(FONT_PATH, 64)
bbox = draw.textbbox((0, 0), TEXT, font=font)
tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
draw.text(((W - tw) / 2 - bbox[0], (H - th) / 2 - bbox[1]), TEXT, font=font, fill=(255, 255, 255))

# simple border so we can confirm the whole frame is being scanned out correctly
draw.rectangle([4, 4, W - 5, H - 5], outline=(255, 128, 0), width=4)

img.save("/tmp/claude-1000/-media-sf-GitHub-prado-firmware-reconstruction/fe153a6b-de79-4463-be25-87cd8480fa0b/scratchpad/test_bootlogo.png")
print("wrote test_bootlogo.png")
