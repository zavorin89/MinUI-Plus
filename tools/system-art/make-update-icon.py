#!/usr/bin/env python3
"""Makes the "Update Available!" art (.res/Update Available!.png) in the style of Favorites, Recently Played and
Tools: their shared background (black, the slanted grey panel), and a light line icon of an arrow down into a tray.

usage: make-update-icon.py FAVORITES RECENTLY_PLAYED TOOLS OUTPUT   (needs Pillow)

The background comes from the references themselves: each pixel from one whose icon isn't there (or, under all
three, the nearest clean pixel in its column), so the panel's edge and its antialiasing are exactly theirs. The icon is drawn 4x and scaled down for smooth 9px rounded lines."""
import sys
from PIL import Image, ImageDraw, ImageFilter

refs = [Image.open(p).convert('RGB') for p in sys.argv[1:4]]
w, h = refs[0].size
# where each reference's icon is (light pixels, grown a little for their antialiasing)
masks = [r.convert('L').point(lambda v: 255 if v > 44 else 0).filter(ImageFilter.MaxFilter(9)) for r in refs]
out = Image.new('RGB', (w, h))
px, mpx, rpx = out.load(), [m.load() for m in masks], [r.load() for r in refs]
for y in range(h):
	for x in range(w):
		for i in range(len(refs)):
			if not mpx[i][x, y]:
				px[x, y] = rpx[i][x, y]
				break
		else: # under every icon: the nearest clean pixel in this column (the panel's edge barely moves row to row)
			for dy in range(1, h):
				found = None
				for yy in (y - dy, y + dy):
					if 0 <= yy < h:
						for i in range(len(refs)):
							if not mpx[i][x, yy]:
								found = rpx[i][x, yy]
								break
					if found: break
				if found:
					px[x, y] = found
					break

S = 4 # drawn at 4x
ink = (224, 224, 224)
stroke = 9 * S # as heavy as the star's
lines = [
	[(120, 170), (120, 262)], # the arrow's shaft
	[(89, 231), (120, 262), (151, 231)], # its head
	[(62, 262), (62, 298), (178, 298), (178, 262)], # the tray
]
layer = Image.new('L', (w * S, h * S), 0)
d = ImageDraw.Draw(layer)
for line in lines:
	pts = [(x * S, y * S) for x, y in line]
	d.line(pts, fill=255, width=stroke, joint='curve')
	for x, y in (pts[0], pts[-1]): # round ends
		r = stroke / 2
		d.ellipse((x - r, y - r, x + r, y + r), fill=255)
alpha = layer.resize((w, h), Image.LANCZOS)
out.paste(Image.new('RGB', (w, h), ink), (0, 0), alpha)
out.save(sys.argv[4], optimize=True)
