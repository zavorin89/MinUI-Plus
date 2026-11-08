"""The arcade systems' borders (Bezels/<TAG>.png), made for MinUI+ in the style of drkhrse's: dark charcoal bands with
a red power light on the left and the system's name up the right, in MinUI's own font (BPreplay Bold).

Arcade games fill the A30's 4:3 screen, so these only show around vertical games: the see-through window is a 3:4
game at full height (140, 0, 360 x 480), and minarch moves the bands in or out to meet a game of another shape.

Run with the system-art venv (it has Pillow):  work/system-art-venv/bin/python tools/borders/make-arcade-borders.py
Writes tools/borders/generated/<TAG>.png and copies them to skeleton/EXTRAS/Bezels/."""
import os
import random
import shutil
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FONT = os.path.join(REPO, "skeleton/SYSTEM/res/BPreplayBold-unhinted.otf")
W, H = 640, 480
GAME_X, GAME_W = 140, 360  # 3:4 at full height

# TAG: (the small line, the big line, the big line's colors top to bottom)
SYSTEMS = {
	"CPS1": ("CAPCOM PLAY SYSTEM", "CPS-1", ((255, 214, 92), (232, 140, 30))),
	"CPS2": ("CAPCOM PLAY SYSTEM", "CPS-2", ((120, 180, 255), (40, 96, 214))),
	"CPS3": ("CAPCOM PLAY SYSTEM", "CPS-3", ((206, 160, 255), (124, 70, 210))),
	"FBN": ("FINAL BURN", "NEO", ((255, 170, 80), (226, 72, 30))),
	"MAME": ("MULTIPLE ARCADE", "MAME", ((255, 120, 110), (196, 34, 40))),
}


def band():
	"""the charcoal: near black, a faint vertical sheen and fine grain, like the other borders"""
	random.seed(7)
	img = Image.new("RGB", (W, H))
	px = img.load()
	for y in range(H):
		for x in range(W):
			v = 16 + random.randint(-2, 2)
			px[x, y] = (v, v, v)
	return img


def inner_edges(draw):
	"""a hairline where each band meets the game, as a bezel's lip"""
	for x, shade in ((GAME_X - 2, 34), (GAME_X - 1, 8), (GAME_X + GAME_W, 8), (GAME_X + GAME_W + 1, 34)):
		draw.line([(x, 0), (x, H)], fill=(shade, shade, shade))


def power_light(img):
	"""a red light and a grey POWER under it, centred in the left band, as on the handheld borders"""
	cx, cy = GAME_X // 2, 196
	glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
	ImageDraw.Draw(glow).ellipse([cx - 18, cy - 18, cx + 18, cy + 18], fill=(255, 30, 30, 90))
	glow = glow.filter(ImageFilter.GaussianBlur(8))
	img.paste(glow, (0, 0), glow)
	d = ImageDraw.Draw(img)
	d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=(150, 10, 12))
	d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=(225, 30, 30))
	d.ellipse([cx - 4, cy - 5, cx, cy - 1], fill=(255, 150, 150))
	font = ImageFont.truetype(FONT, 13)
	w = d.textlength("POWER", font=font)
	d.text((cx - w / 2, cy + 16), "POWER", font=font, fill=(150, 150, 150))


def gradient_text(text, font, colors):
	"""text as an RGBA image, its fill running from colors[0] at the top to colors[1] at the bottom, with a dark edge"""
	box = font.getbbox(text)
	w, h = box[2] - box[0] + 8, box[3] - box[1] + 8
	mask = Image.new("L", (w, h), 0)
	ImageDraw.Draw(mask).text((4 - box[0], 4 - box[1]), text, font=font, fill=255)
	fill = Image.new("RGB", (w, h))
	fd = ImageDraw.Draw(fill)
	for y in range(h):
		t = y / max(1, h - 1)
		fd.line([(0, y), (w, y)], fill=tuple(int(colors[0][i] + (colors[1][i] - colors[0][i]) * t) for i in range(3)))
	out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
	edge = mask.filter(ImageFilter.MaxFilter(3))
	out.paste((0, 0, 0, 255), (0, 0), edge)
	out.paste(fill, (0, 0), mask)
	return out


def wordmark(img, small, big, colors):
	"""the system's name reading up the right band: the big line in its colors, the small one grey beside it"""
	big_font = ImageFont.truetype(FONT, 78)
	small_font = ImageFont.truetype(FONT, 17)
	big_img = gradient_text(big, big_font, colors)
	sw = int(small_font.getlength(small)) + 8
	small_img = Image.new("RGBA", (sw, 24), (0, 0, 0, 0))
	ImageDraw.Draw(small_img).text((4, 2), small, font=small_font, fill=(128, 128, 128))
	# side by side while upright (small above big), then turned a quarter counter-clockwise to read bottom to top
	gap = 4
	w = max(big_img.width, small_img.width)
	block = Image.new("RGBA", (w, small_img.height + gap + big_img.height), (0, 0, 0, 0))
	block.paste(small_img, ((w - small_img.width) // 2, 0), small_img)
	block.paste(big_img, ((w - big_img.width) // 2, small_img.height + gap), big_img)
	block = block.rotate(90, expand=True, resample=Image.BICUBIC)
	band_x = GAME_X + GAME_W + 2
	band_w = W - band_x
	scale = min(1.0, (H - 60) / block.height, (band_w - 24) / block.width)
	if scale < 1:
		block = block.resize((int(block.width * scale), int(block.height * scale)), Image.LANCZOS)
	img.paste(block, (band_x + (band_w - block.width) // 2, (H - block.height) // 2), block)


def make(tag):
	small, big, colors = SYSTEMS[tag]
	img = band().convert("RGBA")
	inner_edges(ImageDraw.Draw(img))
	power_light(img)
	wordmark(img, small, big, colors)
	# the window the game shows through
	ImageDraw.Draw(img).rectangle([GAME_X, 0, GAME_X + GAME_W - 1, H - 1], fill=(0, 0, 0, 0))
	return img


def main():
	out_dir = os.path.join(HERE, "generated")
	ship_dir = os.path.join(REPO, "skeleton/EXTRAS/Bezels")
	os.makedirs(out_dir, exist_ok=True)
	for tag in SYSTEMS:
		path = os.path.join(out_dir, tag + ".png")
		make(tag).save(path, optimize=True)
		shutil.copy(path, os.path.join(ship_dir, tag + ".png"))
		print("made", path)


if __name__ == "__main__":
	main()
