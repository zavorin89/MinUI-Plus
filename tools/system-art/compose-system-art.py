#!/usr/bin/env python3
"""Compose a transparent hardware cutout into MinUI's Nao-Black system-art layout.

The editable source is 1024x768. The shipped A30 asset is the rightmost 271x480 strip after the source is scaled
to 640x480, matching tools/system-art/install.sh and the existing system art.

usage: compose-system-art.py INPUT SOURCE_OUTPUT STRIP_OUTPUT PANEL_COLOR MAX_WIDTH MAX_HEIGHT [CENTER_X CENTER_Y]
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw


SOURCE_SIZE = (1024, 768)
SCALED_SIZE = (640, 480)
STRIP_BOX = (369, 0, 640, 480)
PANEL_POLYGON = ((882, 0), (1024, 0), (1024, 768), (836, 768))


def fail(message):
	print(message, file=sys.stderr)
	raise SystemExit(1)


if len(sys.argv) not in (7, 9):
	fail(__doc__.strip())

input_path = Path(sys.argv[1])
source_path = Path(sys.argv[2])
strip_path = Path(sys.argv[3])
panel_color = sys.argv[4]
max_width = int(sys.argv[5])
max_height = int(sys.argv[6])
center_x = int(sys.argv[7]) if len(sys.argv) == 9 else 790
center_y = int(sys.argv[8]) if len(sys.argv) == 9 else 384

cutout = Image.open(input_path).convert("RGBA")
alpha = cutout.getchannel("A")
visible = alpha.point(lambda value: 255 if value > 3 else 0)
bounds = visible.getbbox()
if not bounds:
	fail(f"no visible pixels in {input_path}")
cutout = cutout.crop(bounds)

scale = min(max_width / cutout.width, max_height / cutout.height)
size = (max(1, round(cutout.width * scale)), max(1, round(cutout.height * scale)))
cutout = cutout.resize(size, Image.Resampling.LANCZOS)

source = Image.new("RGB", SOURCE_SIZE, "black")
ImageDraw.Draw(source).polygon(PANEL_POLYGON, fill=panel_color)
x = center_x - cutout.width // 2
y = center_y - cutout.height // 2
source.paste(cutout, (x, y), cutout)

source_path.parent.mkdir(parents=True, exist_ok=True)
strip_path.parent.mkdir(parents=True, exist_ok=True)
source.save(source_path, format="PNG", optimize=True)
scaled = source.resize(SCALED_SIZE, Image.Resampling.LANCZOS)
scaled.crop(STRIP_BOX).save(strip_path, format="PNG", optimize=True)

print(f"{input_path.name}: cutout {cutout.width}x{cutout.height} at ({x},{y}); strip {strip_path}")
