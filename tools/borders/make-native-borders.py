#!/usr/bin/env python3
"""Makes Bezels/<TAG>-native.png: the borders used at native scaling, where the game is smaller than the normal
(aspect) borders leave room for. Each is the handheld's face in its shell color around the game: a dark screen lens,
the logos and power light from drkhrse's borders, and its controls and speaker, drawn small around the edges.

    python3 tools/borders/make-native-borders.py skeleton/EXTRAS/Bezels [TAG...]
writes them next to the normal borders (all of them when no TAG is given). Plain Python, no libraries.
"""
import colorsys
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from borderkit import W, H, read_png, write_png, clamp, mix, rounded_rect_cover, circle_cover, crop_bright, draw_logo

# where the game is at native scaling (integer scale, centered on 640x480)
GB_GAME = (80, 24, 480, 432)  # 160x144 at 3x
NGP_GAME = (80, 12, 480, 456)  # 160x152 at 3x
GBA_GAME = (80, 80, 480, 320)  # 240x160 at 2x

GB_LENS = (62, 6, 516, 468)  # the game plus a narrow margin: there's little room above and below at 3x
GB_FACE_LENS = (20, 6, 600, 468)  # wide enough to hold the power light on the left and the logo on the right
NGP_LENS = (64, 2, 512, 476)

# placed pieces: (border file, its box, center x, center y, how)
# how: "keep" its own colors, "tint" in the shell's text color, "red" keeps red (power lights, logos) and tints the rest
SYSTEMS = {
	"GBA": {  # the launch model's indigo
		"game": GBA_GAME, "lens": (50, 30, 540, 420), "radius": 18,
		"shell": ((84, 74, 168), (50, 42, 116)), "text": (196, 188, 236),
		"pieces": [("Extras/GBA-classic.png", "nintendo", 320, 55, "keep")],
		"logo": ("gba-logo.png", 320, 425, 24),  # the holographic one, under the screen
		"led": (25, 62, (40, 255, 90)), "power": (25, 80),
		"dpad": (25, 236, (36, 32, 72)),
		"buttons": [(620, 232, 10, (44, 38, 86)), (603, 258, 10, (44, 38, 86))],
		"pills": [(13, 300, 24, 7, (40, 36, 80)), (13, 318, 24, 7, (40, 36, 80))],
		"grille": (604, 366, 3, 6, (18, 14, 44)),
	},
	# the Game Boys' top half: a big dark lens holding the screen, the power light and the logo, the shell around it
	"GB": {  # the Game Boy Pocket, in silver
		"game": GB_GAME, "lens": GB_FACE_LENS, "radius": 16,
		"shell": ((208, 208, 212), (164, 164, 170)), "lens_color": (52, 54, 60), "text": (64, 64, 74),
		"pieces": [("GB.png", (7, 148, 49, 182), 51, 72, "power"), ("GB.png", (603, 259, 626, 464), 589, 240, "vivid")],
	},
	"GBC": {  # Kiwi lime
		"game": GB_GAME, "lens": GB_FACE_LENS, "radius": 16,
		"shell": ((178, 222, 34), (138, 184, 18)), "lens_color": (54, 58, 78), "text": (44, 56, 20),
		"pieces": [("GBC.png", (6, 175, 48, 211), 51, 72, "power")],
		"gbc_logo": (589, 240, 34),  # center, and the height of COLOR's letters
	},
	"GG": {  # black, as it was
		"game": GB_GAME, "lens": GB_LENS, "radius": 12,
		"shell": ((46, 46, 50), (24, 24, 28)), "lens_color": (12, 12, 14), "text": (150, 150, 156),
		"pieces": [("GG.png", (6, 27, 47, 189), 31, 108, "keep"), ("GG.png", (598, 14, 627, 188), 612, 108, "keep")],
		"dpad": (31, 300, (34, 34, 38)),
		"buttons": [(618, 290, 10, (34, 34, 38)), (600, 316, 10, (34, 34, 38))],
		"start": (610, 230, 6, (40, 70, 190)),  # the blue Start
		"grille": (600, 410, 3, 5, (10, 10, 12)),
	},
	"NGP": {  # carbon black, with its clicky thumb stick
		"game": NGP_GAME, "lens": NGP_LENS, "radius": 8,
		"shell": ((64, 64, 70), (36, 36, 40)), "lens_color": (12, 12, 14), "text": (170, 170, 176),
		"pieces": [("GBC.png", (6, 175, 48, 211), 32, 60, "keep"), ("NGP.png", (585, 125, 621, 348), 608, 320, "keep")],
		"stick": (32, 236),
		"buttons": [(616, 96, 10, (26, 26, 30)), (598, 122, 10, (26, 26, 30))],
		"grille": (14, 404, 4, 5, (22, 22, 26)),
	},
	"NGPC": {  # Platinum Silver
		"game": NGP_GAME, "lens": NGP_LENS, "radius": 8,
		"shell": ((200, 202, 208), (150, 152, 160)), "lens_color": (40, 42, 48), "text": (60, 62, 72),
		"pieces": [("GBC.png", (6, 175, 48, 211), 32, 60, "red"), ("NGPC.png", (585, 125, 621, 348), 608, 320, "red"),
			("NGPC.png", "color", 32, 400, "red")],
		"stick": (32, 236),
		"buttons": [(616, 96, 10, (64, 66, 76)), (598, 122, 10, (64, 66, 76))],
	},
}
SAME_AS = {"MGBA": "GBA"}


class Face:
	def __init__(self, cfg, folder, seed):
		self.cfg = cfg
		self.folder = folder
		self.sources = {}
		self.rng = random.Random(seed)
		self.out = [[(0, 0, 0, 255)] * W for _ in range(H)]

	def source(self, name):
		if name not in self.sources:
			self.sources[name] = read_png(os.path.join(self.folder, name))[2]
		return self.sources[name]

	def blend(self, x, y, color, alpha):
		if alpha <= 0 or x < 0 or y < 0 or x >= W or y >= H:
			return
		r, g, b, a = self.out[y][x]
		alpha = min(1.0, alpha)
		self.out[y][x] = (clamp(r + (color[0] - r) * alpha), clamp(g + (color[1] - g) * alpha), clamp(b + (color[2] - b) * alpha), a)

	def shell(self):
		top, bottom = self.cfg["shell"]
		for y in range(H):
			base = mix(top, bottom, y / (H - 1))  # lit from above
			for x in range(W):
				edge = min(x, W - 1 - x) / 60.0
				shade = 1.0 - 0.08 * max(0.0, 1.0 - edge)  # a little darker towards the sides
				grain = self.rng.uniform(-3, 3)  # fine plastic grain
				self.out[y][x] = tuple(clamp(v * shade + grain) for v in base) + (255,)

	def lens(self):
		lens, radius = self.cfg["lens"], self.cfg["radius"]
		color = self.cfg.get("lens_color") or self.source(self.cfg["pieces"][0][0])[13][10][:3]
		lx, ly, lw, lh = lens
		light = max(self.cfg["shell"][0])
		shadow = (20, 16, 50) if light < 150 else tuple(int(v * 0.55) for v in self.cfg["shell"][1])
		for y in range(ly - 2, ly + lh + 8):
			for x in range(lx - 6, lx + lw + 6):
				self.blend(x, y, shadow, rounded_rect_cover(x - 2, y - 4, lens, radius) * 0.45)
		for y in range(ly - 1, ly + lh + 1):
			for x in range(lx - 1, lx + lw + 1):
				cover = rounded_rect_cover(x, y, lens, radius)
				if not cover:
					continue
				sheen = max(0.0, 1.0 - ((x - lx) / lw + (y - ly) / lh)) * 10  # light from the top left
				self.blend(x, y, tuple(clamp(v + sheen) for v in color), cover)
				if y - ly < 2 and cover > 0.5:
					self.blend(x, y, (150, 150, 170), 0.3)  # its top edge catches the light
		gx, gy, gw, gh = self.cfg["game"]
		for y in range(gy - 3, gy + gh + 3):
			for x in range(gx - 3, gx + gw + 3):
				d = max(gx - x, x - (gx + gw - 1), gy - y, y - (gy + gh - 1))
				if 0 < d <= 3:
					self.blend(x, y, (0, 0, 0), 0.35 * (4 - d) / 3)  # the game sits behind the glass

	def piece(self, name, box, cx, cy, how):
		src = self.source(name)
		if box == "nintendo":
			box = crop_bright(src, (200, 0, 440, 27), 40)
		elif box == "advance":
			box = crop_bright(src, (150, 453, 490, 480), 40)
		elif box == "color":  # NGPC's red "COLOR", at the top of its left band
			box = (16, 2, 50, 128)
		if not box:
			return
		x0, y0, x1, y1 = box
		bg = src[y0][max(0, x0 - 3)][:3]
		ox, oy = int(cx - (x1 - x0) / 2), int(cy - (y1 - y0) / 2)
		if how in ("vivid", "power"):
			self.vivid(src, box, bg, ox, oy, power=how=="power")
			return
		for y in range(y0, y1):
			for x in range(x0, x1):
				r, g, b, a = src[y][x]
				lift = max(r - bg[0], g - bg[1], b - bg[2], 0) / 255
				red = r > g + 40 and r > b + 40
				color = (r, g, b) if how in ("keep", "crisp") or (how == "red" and red) else self.cfg["text"]
				if how == "red" and red:
					color = (min(255, r + 40), g, b)  # a touch brighter to hold up on a light shell
				strength = 2.4 if how == "crisp" else 1.4  # crisp: full strength, the logo as it is on the device
				self.blend(ox + x - x0, oy + y - y0, color, lift * strength)

	# a logo the source draws dim (to sit quietly in a dark band), drawn as the device has it: grey lettering light,
	# colored letters at full color, each pixel's strength from how bright it is next to its color's brightest
	def vivid(self, src, box, bg, ox, oy, power=False):
		x0, y0, x1, y1 = box
		def kind(r, g, b):
			h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
			return ("gray", h) if s < 0.25 else (round(h * 12) % 12, h)
		peak = {}
		for y in range(y0, y1):
			for x in range(x0, x1):
				r, g, b, a = src[y][x]
				k, _ = kind(r, g, b)
				peak[k] = max(peak.get(k, 0), max(r, g, b) - max(bg))
		reds = []
		light = None
		if power:  # where the light is: it's drawn glowing, and nothing of the source right around it is kept
			found = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if kind(*src[y][x][:3])[0] in (0, 11, 1) and max(src[y][x][:3]) - max(bg) > 20]
			if found:
				light = (sum(p[0] for p in found) / len(found), sum(p[1] for p in found) / len(found))
		for y in range(y0, y1):
			for x in range(x0, x1):
				r, g, b, a = src[y][x]
				k, h = kind(r, g, b)
				lift = max(r, g, b) - max(bg)
				if lift <= 0 or peak.get(k, 0) <= 8:
					continue
				if light and k == "gray" and math.hypot(x - light[0], y - light[1]) < 9:
					continue  # (the source has a light streak under it)
				if k == "gray":
					color = (240, 242, 248) if power else (200, 208, 226)  # power: white marks; logos: the real one's light blue-grey
				else:
					color = tuple(int(v * 255) for v in colorsys.hls_to_rgb(h, 0.58, 0.9))
				if power and k in (0, 11, 1):
					reds.append((ox + x - x0, oy + y - y0))
					continue  # the light is drawn below, glowing
				self.blend(ox + x - x0, oy + y - y0, color, lift / peak[k])
		if power and reds:
			cx = sum(p[0] for p in reds) / len(reds) + 0.5
			cy = sum(p[1] for p in reds) / len(reds) + 0.5
			self.glow(cx, cy, (255, 40, 40))

	# a power light that looks lit: a wide soft glow, a bright core and a hot center
	def glow(self, cx, cy, color):
		halo = tuple(min(255, v + 40) for v in color)
		for y in range(int(cy) - 14, int(cy) + 15):
			for x in range(int(cx) - 14, int(cx) + 15):
				d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
				self.blend(x, y, halo, max(0.0, 0.75 * (1 - d / 14)) ** 1.6)
				self.blend(x, y, color, circle_cover(x, y, cx, cy, 3.8))
				self.blend(x, y, (255, 200, 190), circle_cover(x, y, cx, cy, 2.2))
				self.blend(x, y, (255, 255, 250), circle_cover(x, y, cx - 0.6, cy - 0.6, 1.1))

	def led(self):
		if "led" not in self.cfg:
			return
		cx, cy, color = self.cfg["led"]
		self.glow(cx + 0.5, cy + 0.5, color)
		if "power" in self.cfg:
			src = self.source("GBC.png")
			label = crop_bright(src, (0, 196, 52, 216), 60)
			if label:
				px, py = self.cfg["power"]
				x0, y0, x1, y1 = label
				ox, oy = int(px - (x1 - x0) / 2), int(py - (y1 - y0) / 2)
				peak = max(max(src[y][x][:3]) for y in range(y0, y1) for x in range(x0, x1)) - 9
				for y in range(y0, y1):
					for x in range(x0, x1):
						r, g, b, a = src[y][x]
						self.blend(ox + x - x0, oy + y - y0, (244, 244, 250), max(max(r, g, b) - 9, 0) / peak)  # white, full strength

	def raised(self, cover_at, box, color):  # molded in, lit from above, a soft shadow under
		x0, y0, x1, y1 = box
		for y in range(y0, y1):
			for x in range(x0, x1):
				c = cover_at(x, y)
				if not c:
					continue
				light = 18 if not cover_at(x, y - 2) else (-16 if not cover_at(x, y + 2) else 0)
				self.blend(x, y, tuple(clamp(v + light) for v in color), c)
				self.blend(x, y + 3, (10, 8, 24), (1 - cover_at(x, y + 3)) * c * 0.35)

	def controls(self):
		cfg = self.cfg
		if "dpad" in cfg:
			cx, cy, color = cfg["dpad"]
			cross = lambda x, y: max(rounded_rect_cover(x, y, (cx - 18, cy - 6, 36, 12), 3), rounded_rect_cover(x, y, (cx - 6, cy - 18, 12, 36), 3))
			self.raised(cross, (cx - 21, cy - 22, cx + 22, cy + 23), color)
		if "stick" in cfg:  # a ring with a round cap and a dimple
			cx, cy = cfg["stick"]
			dark = tuple(int(v * 0.5) for v in cfg["shell"][1])
			for y in range(cy - 24, cy + 25):
				for x in range(cx - 24, cx + 25):
					self.blend(x, y, dark, circle_cover(x, y, cx, cy, 20))
			cap = tuple(clamp(v * 0.8) for v in cfg["shell"][0])
			self.raised(lambda x, y: circle_cover(x, y, cx, cy, 13), (cx - 15, cy - 15, cx + 16, cy + 16), cap)
			for y in range(cy - 6, cy + 7):
				for x in range(cx - 6, cx + 7):
					self.blend(x, y, dark, circle_cover(x, y, cx, cy + 1, 4) * 0.5)
		for (cx, cy, r, color) in cfg.get("buttons", []):
			self.raised(lambda x, y, cx=cx, cy=cy, r=r: circle_cover(x, y, cx, cy, r), (cx - r - 2, cy - r - 2, cx + r + 3, cy + r + 3), color)
		for (x0, y0, w, h, color) in cfg.get("pills", []):
			self.raised(lambda x, y, x0=x0, y0=y0, w=w, h=h: rounded_rect_cover(x, y, (x0, y0, w, h), h / 2), (x0 - 2, y0 - 2, x0 + w + 3, y0 + h + 4), color)
		if "start" in cfg:
			cx, cy, r, color = cfg["start"]
			self.raised(lambda x, y: circle_cover(x, y, cx, cy, r), (cx - r - 2, cy - r - 2, cx + r + 3, cy + r + 3), color)
		if "grille" in cfg:  # the speaker's holes
			gx, gy, cols, rows, color = cfg["grille"]
			for row in range(rows):
				for col in range(cols):
					cx, cy = gx + col * 9 + (4 if row % 2 else 0), gy + row * 9
					for y in range(cy - 3, cy + 4):
						for x in range(cx - 3, cx + 4):
							self.blend(x, y, color, circle_cover(x, y, cx, cy, 2.2))

	def window(self):
		gx, gy, gw, gh = self.cfg["game"]
		for y in range(gy, gy + gh):
			for x in range(gx, gx + gw):
				self.out[y][x] = (255, 255, 255, 0)

	def plates(self):  # bits of the lens' glass on the shell, for logos that need it dark behind them
		color = self.cfg.get("lens_color", (20, 20, 24))
		for (px, py, pw, ph, r) in self.cfg.get("plates", []):
			rect = (px, py, pw, ph)
			for y in range(py - 2, py + ph + 6):
				for x in range(px - 4, px + pw + 4):
					self.blend(x, y, tuple(int(v * 0.55) for v in self.cfg["shell"][1]), rounded_rect_cover(x - 1, y - 3, rect, r) * 0.45)
			for y in range(py, py + ph):
				for x in range(px, px + pw):
					cover = rounded_rect_cover(x, y, rect, r)
					self.blend(x, y, color, cover)
					if y - py < 2 and cover > 0.5:
						self.blend(x, y, (150, 150, 170), 0.3)

	# the Game Boy Color logo from tools/borders/gbc-logo.png as one line, GAME BOY then COLOR, running up the side:
	# GAME BOY in the light blue-grey it has on the real lens, COLOR in its own colors
	def gbc_logo(self):
		cx, cy, color_h = self.cfg["gbc_logo"]
		w, h, src = read_png(os.path.join(os.path.dirname(os.path.abspath(__file__)), "gbc-logo.png"))
		navy = lambda r, g, b: b > r + 60 and b > g + 60 and r < 70
		def unwhite(r, g, b):  # its white background taken out: the ink's coverage, and its color at full coverage
			a = (255 - min(r, g, b)) / 255
			if a <= 0.02:
				return 0, (0, 0, 0)
			return a, tuple(clamp((v - 255 * (1 - a)) / a) for v in (r, g, b))
		def cut(box, keep):  # the part of the logo in box whose pixels keep() accepts, as coverage and color
			x0, y0, x1, y1 = box
			return [[(lambda a, c: (a, c) if a and keep(*src[y][x][:3]) else (0, (0, 0, 0)))(*unwhite(*src[y][x][:3])) for x in range(x0, x1)] for y in range(y0, y1)]
		tm = lambda x, y: x > 505 and y > 322  # the TM, under the Y's arm
		game_boy = [[(0, (0, 0, 0)) if tm(x, y) else px for x, px in zip(range(20, 545), row)] for y, row in zip(range(250, 354), cut((20, 250, 545, 354), lambda r, g, b: True))]
		color = cut((0, 354, w, 612), lambda r, g, b: not navy(r, g, b))
		def shrink(part, out_h):  # box filtered down to out_h tall
			ph, pw = len(part), len(part[0])
			out_w = max(1, round(pw * out_h / ph))
			out = []
			for oy in range(out_h):
				sy0, sy1 = oy * ph // out_h, max(oy * ph // out_h + 1, (oy + 1) * ph // out_h)
				row = []
				for ox in range(out_w):
					sx0, sx1 = ox * pw // out_w, max(ox * pw // out_w + 1, (ox + 1) * pw // out_w)
					a = r = g = b = 0.0
					n = (sy1 - sy0) * (sx1 - sx0)
					for yy in range(sy0, sy1):
						for xx in range(sx0, sx1):
							pa, pc = part[yy][xx]
							a += pa
							r += pa * pc[0]; g += pa * pc[1]; b += pa * pc[2]
					row.append((a / n, (r / a, g / a, b / a) if a else (0, 0, 0)))
				out.append(row)
			return out
		gb_h = round(color_h * 0.62)
		gb = shrink(game_boy, gb_h)
		co = shrink(color, color_h)
		gap = round(color_h * 0.3)
		line_w = len(gb[0]) + gap + len(co[0])
		line = [[(0, (0, 0, 0))] * line_w for _ in range(color_h)]
		for y in range(gb_h):  # bottoms lined up
			for x in range(len(gb[0])):
				a, c = gb[y][x]
				line[color_h - gb_h + y - 1][x] = (a, (200, 208, 226)) if a else (0, (0, 0, 0))
		for y in range(color_h):
			for x in range(len(co[0])):
				a, c = co[y][x]
				if a:
					h_, l_, s_ = colorsys.rgb_to_hls(*(v / 255 for v in c))
					if 0.55 <= h_ <= 0.8:  # its blue O is dark on the dark glass: lifted, the other letters as they are
						c = tuple(v * 255 for v in colorsys.hls_to_rgb(h_, max(l_, 0.55), s_))
				line[y][len(gb[0]) + gap + x] = (a, c)
		# turned to run up the side: the line's start at the bottom, the letters' tops to the left
		ox, oy = int(cx - color_h / 2), int(cy - line_w / 2)
		for ly in range(color_h):
			for lx in range(line_w):
				a, c = line[ly][lx]
				self.blend(ox + ly, oy + line_w - 1 - lx, c, min(1.0, a * 1.15))

	def make(self):
		self.shell()
		self.lens()
		self.plates()
		for (name, box, cx, cy, how) in self.cfg["pieces"]:
			self.piece(name, box, cx, cy, how)
		if "gbc_logo" in self.cfg:
			self.gbc_logo()
		if "logo" in self.cfg:
			name, cx, cy, height = self.cfg["logo"]
			draw_logo(self.out, os.path.join(os.path.dirname(os.path.abspath(__file__)), name), cx, cy, height)
		self.led()
		self.controls()
		self.window()
		return self.out


def main():
	folder = sys.argv[1] if len(sys.argv) > 1 else "skeleton/EXTRAS/Bezels"
	tags = sys.argv[2:] or list(SYSTEMS) + list(SAME_AS)
	made = {}
	for tag in tags:
		base = SAME_AS.get(tag, tag)
		if base not in made:
			made[base] = Face(SYSTEMS[base], folder, seed=7).make()
		path = os.path.join(folder, f"{tag}-native.png")
		write_png(path, made[base])
		print("wrote", path)


main()
