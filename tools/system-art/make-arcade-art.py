"""The arcade systems' art (CPS1, CPS2, CPS3, Final Burn Neo, MAME, Neo Geo, Sega CD, Sega 32X), laid out like
Nao-Black's: a black ground, the slanted band on the right in the system's color (the Genesis family's dark grid for
Sega CD), and the hardware photo big and level, overlapping the band, at the size and place Nao-Black uses: a console
(with its controller) as wide as theirs, an arcade cabinet as tall as their Pac-Man one. No glows or shadows.

Writes the editable 1024x768 sources to tools/system-art/generated/ and the 271x480 strips (scaled to 640x480, the
right-hand 271, as install.sh does) to skeleton/BASE/Roms/.res/.

Run with the system-art venv (Pillow):  work/system-art-venv/bin/python tools/system-art/make-arcade-art.py
Inputs: work/system-art-arcade/sources/ (Commons photos, see CREDITS.md) and Nao-Black's own Arcade (FBN) wallpaper in
tools/system-art/reference/."""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SOURCES = os.path.join(REPO, "work/system-art-arcade/sources")
OUT_SOURCE = os.path.join(HERE, "generated")
OUT_STRIP = os.path.join(REPO, "skeleton/BASE/Roms/.res")

SOURCE = (1024, 768)
STRIP_BOX = (369, 0, 640, 480)  # of the 640x480 scale
BAND = ((882, 0), (1024, 0), (1024, 768), (836, 768))  # Nao-Black's band, at 181 to 151 in the strip
# where Nao-Black's hardware sits, in source pixels: consoles about 226x165 in the strip, centered at (117, 240);
# their Pac-Man cabinet about 230 tall, centered at (143, 240)
CONSOLE = dict(max_w=362, max_h=275, cx=777, cy=388)
CABINET = dict(max_w=210, max_h=368, cx=819, cy=384)


def strip_white(path, size=1400, thresh=18):
	"""a product photo on white, cut out: white flooded from the edges, its edge softened by half a pixel"""
	im = Image.open(path).convert("RGB")
	im.thumbnail((size, size), Image.LANCZOS)
	key = (255, 0, 255)
	flood = im.copy()
	w, h = flood.size
	for x in range(0, w, 8):
		for y in (0, h - 1):
			if sum(flood.getpixel((x, y))) > 3 * (255 - thresh): ImageDraw.floodfill(flood, (x, y), key, thresh=thresh)
	for y in range(0, h, 8):
		for x in (0, w - 1):
			if sum(flood.getpixel((x, y))) > 3 * (255 - thresh): ImageDraw.floodfill(flood, (x, y), key, thresh=thresh)
	mask = Image.eval(flood.convert("RGB").split()[1], lambda v: 0)  # placeholder size
	fp = flood.load()
	mp = mask.load()
	for y in range(h):
		for x in range(w):
			mp[x, y] = 0 if fp[x, y] == key else 255
	mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.7))
	out = im.convert("RGBA")
	out.putalpha(mask)
	return out


def solid(path, min_alpha=200):
	"""a transparent cutout without any soft drop shadow (its faint pixels dropped)"""
	im = Image.open(path).convert("RGBA")
	a = im.getchannel("A").point(lambda v: 255 if v >= min_alpha else (0 if v < 96 else v))
	im.putalpha(a)
	return im


def traced(path, outline):
	"""a photo with a busy background, cut out along an outline traced by hand, in thousandths of the photo's height"""
	im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
	im.thumbnail((1400, 1400), Image.LANCZOS)
	scale = im.height / 1000 * 2  # the mask is drawn at twice the size, for a smooth edge
	mask = Image.new("L", (im.width * 2, im.height * 2), 0)
	ImageDraw.Draw(mask).polygon([(x * scale, y * scale) for x, y in outline], fill=255)
	out = im.convert("RGBA")
	out.putalpha(mask.resize(im.size, Image.LANCZOS))
	return out


def trimmed(im):
	box = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
	return im.crop(box)


def with_controller(console, controller, scale=0.42, dx=0.02, dy=0.58):
	"""a console with its controller in front, lower left, as Nao-Black's sets are"""
	console, controller = trimmed(console), trimmed(controller)
	cw = int(console.width * scale)
	controller = controller.resize((cw, int(controller.height * cw / controller.width)), Image.LANCZOS)
	x, y = int(console.width * dx), int(console.height * dy)
	w = max(console.width, x + controller.width)
	h = max(console.height, y + controller.height)
	out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
	out.paste(console, (w - console.width, 0), console)
	out.paste(controller, (x, y), controller)
	return out


def compose(subject, color, place, grid=False):
	src = Image.new("RGB", SOURCE, "black")
	d = ImageDraw.Draw(src)
	d.polygon(BAND, fill=color)
	if grid:  # the Genesis family's dark band: a line every 30 strip pixels, as on Nao-Black's Genesis
		band = Image.new("L", SOURCE, 0)
		ImageDraw.Draw(band).polygon(BAND, fill=255)
		lines = Image.new("RGB", SOURCE, color)
		ld = ImageDraw.Draw(lines)
		for i in range(30):
			x = round(868.8 + i * 48)
			ld.line([(x, 0), (x, SOURCE[1])], fill=(84, 84, 84), width=2)
			y = round(46.4 + i * 48)
			ld.line([(0, y), (SOURCE[0], y)], fill=(84, 84, 84), width=2)
		src.paste(lines, (0, 0), band)
	subject = trimmed(subject)
	s = min(place["max_w"] / subject.width, place["max_h"] / subject.height)
	subject = subject.resize((round(subject.width * s), round(subject.height * s)), Image.LANCZOS)
	src.paste(subject, (place["cx"] - subject.width // 2, place["cy"] - subject.height // 2), subject)
	return src


def save(name, src):
	os.makedirs(OUT_SOURCE, exist_ok=True)
	src.save(os.path.join(OUT_SOURCE, name), optimize=True)
	strip = src.resize((640, 480), Image.LANCZOS).crop(STRIP_BOX)
	strip.save(os.path.join(OUT_STRIP, name), optimize=True)
	print("made", name)


ARCADE_CLASSICS = [(40,197),(297,205),(300,270),(445,480),(530,535),(532,550),(520,600),(458,607),(445,770),(400,810),(232,955),(215,940),(100,815),(80,700),(30,355),(15,262),(15,225)]
ASTRO_CITY = [(62,14),(385,14),(402,22),(432,130),(447,150),(472,260),(525,530),(532,558),(615,590),(642,625),(630,650),(530,700),(518,730),(520,850),(505,890),(170,990),(150,960),(125,920),(110,850),(100,720),(75,600),(50,450),(20,250),(10,150),(30,100),(40,30)]
NEW_ASTRO_CITY = [(170,112),(420,103),(450,110),(470,135),(495,205),(500,225),(520,300),(570,520),(575,550),(660,595),(675,620),(660,645),(575,690),(560,770),(560,800),(330,905),(305,912),(285,895),(265,870),(240,780),(225,680),(210,610),(190,530),(130,235),(130,215),(150,140)]
CUSTOM_MULTIGAME = [(175,100),(490,118),(485,160),(470,210),(460,260),(470,320),(490,400),(510,440),(590,485),(593,505),(580,520),(530,540),(525,760),(532,770),(530,835),(250,915),(218,905),(93,790),(93,712),(100,700),(100,350),(95,210),(120,118)]


def main():
	pad = strip_white(os.path.join(SOURCES, "Sega-Genesis-6But-Cont.jpg"))
	sega_cd = strip_white(os.path.join(SOURCES, "Sega-CD-Model1-Set.jpg"))
	x32 = Image.open(os.path.join(SOURCES, "Sega-Genesis-Model2-32X.png")).convert("RGBA")
	aes = Image.open(os.path.join(SOURCES, "Neo-Geo-AES-Console-Set.png")).convert("RGBA")
	aes = with_controller(aes.crop((1900, 0, aes.width, aes.height)), aes.crop((0, 0, 1760, aes.height)), scale=0.52, dx=0, dy=0.5)  # the stick in front, not beside

	save("Neo Geo (NEOGEO).png", compose(aes, (168, 132, 48), CONSOLE))
	save("Sega CD (SEGACD).png", compose(sega_cd, (26, 26, 26), CONSOLE, grid=True))
	save("Sega 32X (THIRTYTWOX).png", compose(with_controller(x32, pad), (224, 190, 67), CONSOLE))

	save("Capcom Play System 1 (CPS1).png", compose(traced(os.path.join(SOURCES, "Borne d'arcade.jpg"), ARCADE_CLASSICS), (52, 96, 168), CABINET))
	save("Capcom Play System 2 (CPS2).png", compose(traced(os.path.join(SOURCES, "Sega astro city candy cab.jpg"), ASTRO_CITY), (40, 128, 88), CABINET))
	save("Capcom Play System 3 (CPS3).png", compose(traced(os.path.join(SOURCES, "Sega New Astro City (50356520032).jpg"), NEW_ASTRO_CITY), (124, 72, 168), CABINET))
	save("MAME 2003+ (MAME).png", compose(traced(os.path.join(SOURCES, "\"Custom\" multigame arcade cabinet (2137119547).jpg"), CUSTOM_MULTIGAME), (40, 118, 140), CABINET))
	# Final Burn Neo: Nao-Black's own arcade picture (their Pac-Man cabinet), as it is
	save("Final Burn Neo (FBN).png", Image.open(os.path.join(HERE, "reference", "Arcade (FBN).png")).convert("RGB").resize(SOURCE, Image.LANCZOS))


if __name__ == "__main__":
	main()
