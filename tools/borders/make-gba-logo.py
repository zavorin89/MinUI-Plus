#!/usr/bin/env python3
"""Puts the holographic GAME BOY ADVANCE logo (tools/borders/gba-logo.png) into the bottom bar of the GBA border used
at aspect scaling, in place of the one drawn in drkhrse's, and makes the Nintendo logo up top as subtle as on the
native border: their original is kept as Extras/GBA-classic.png and this
works from that, so it can be run again. Writes GBA.png and MGBA.png (the same).

    python3 tools/borders/make-gba-logo.py skeleton/EXTRAS/Bezels
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from borderkit import read_png, write_png, crop_bright, draw_logo

folder = sys.argv[1] if len(sys.argv) > 1 else "skeleton/EXTRAS/Bezels"
classic = os.path.join(folder, "Extras", "GBA-classic.png")
if not os.path.exists(classic):
	shutil.copyfile(os.path.join(folder, "GBA.png"), classic)
_, _, rows = read_png(classic)

# the old logo painted over with the bar's own shading (each row is one shade across)
x0, y0, x1, y1 = crop_bright(rows, (60, 454, 580, 480), 25)
for y in range(max(y0 - 1, 455), 480):
	shade = rows[y][40]
	for x in range(x0 - 4, x1 + 4):
		rows[y][x] = shade
draw_logo(rows, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gba-logo.png"), 320, 467, 20)

# the Nintendo logo in the top bar, darker: as subtle as it is on the native border's lens
x0, y0, x1, y1 = crop_bright(rows, (200, 0, 440, 27), 40)
for y in range(max(y0 - 2, 0), min(y1 + 2, 27)):
	shade = rows[y][150]
	for x in range(x0 - 2, x1 + 2):
		r, g, b, a = rows[y][x]
		rows[y][x] = tuple(int(shade[i] + max(0, (r, g, b)[i] - shade[i]) * 0.33) for i in range(3)) + (a,)
for name in ("GBA.png", "MGBA.png"):
	write_png(os.path.join(folder, name), rows)
	print("wrote", os.path.join(folder, name))
