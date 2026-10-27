"""Shared bits for the border generators: PNG reading and writing in plain Python, and antialiased shapes."""
import math
import struct
import zlib

W, H = 640, 480

def read_png(path):
	data = open(path, "rb").read()
	i = 8
	idat = b""
	while i < len(data):
		length = struct.unpack(">I", data[i:i + 4])[0]
		kind = data[i + 4:i + 8]
		chunk = data[i + 8:i + 8 + length]
		if kind == b"IHDR":
			w, h, depth, color = struct.unpack(">IIBB", chunk[:10])
		if kind == b"IDAT":
			idat += chunk
		i += 12 + length
	raw = zlib.decompress(idat)
	bpp = {6: 4, 2: 3}[color]
	stride = w * bpp
	rows = []
	prev = bytearray(stride)
	p = 0
	for y in range(h):
		f = raw[p]
		p += 1
		line = bytearray(raw[p:p + stride])
		p += stride
		for x in range(stride):
			a = line[x - bpp] if x >= bpp else 0
			b = prev[x]
			c = prev[x - bpp] if x >= bpp else 0
			if f == 1: line[x] = (line[x] + a) & 255
			elif f == 2: line[x] = (line[x] + b) & 255
			elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
			elif f == 4:
				pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
				line[x] = (line[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
		rows.append([tuple(line[x * bpp:x * bpp + 3]) + ((line[x * bpp + 3],) if bpp == 4 else (255,)) for x in range(w)])
		prev = line
	return w, h, rows


def write_png(path, pixels):
	raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in pixels)
	def chunk(kind, body):
		return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
	with open(path, "wb") as f:
		f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
			+ chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def clamp(v):
	return 0 if v < 0 else (255 if v > 255 else int(v))


def mix(a, b, t):
	return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rounded_rect_cover(x, y, rect, r):
	"""0-1 coverage of pixel (x, y) by a rounded rectangle, 4x4 supersampled near the corners"""
	rx, ry, rw, rh = rect
	if x < rx - 1 or y < ry - 1 or x > rx + rw or y > ry + rh:
		return 0.0
	inside = 0
	for sy in range(4):
		for sx in range(4):
			px, py = x + (sx + 0.5) / 4, y + (sy + 0.5) / 4
			dx = max(rx + r - px, 0, px - (rx + rw - r))
			dy = max(ry + r - py, 0, py - (ry + rh - r))
			if px >= rx and px <= rx + rw and py >= ry and py <= ry + rh and dx * dx + dy * dy <= r * r:
				inside += 1
	return inside / 16


def circle_cover(x, y, cx, cy, r):
	d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
	return max(0.0, min(1.0, r - d + 0.5))


def crop_bright(rows, box, threshold):
	"""the bounding box of pixels brighter than threshold inside box (x0, y0, x1, y1)"""
	x0, y0, x1, y1 = box
	xs, ys = [], []
	for y in range(y0, y1):
		for x in range(x0, x1):
			r, g, b, a = rows[y][x]
			if a > 128 and (r + g + b) / 3 > threshold:
				xs.append(x)
				ys.append(y)
	return (min(xs), min(ys), max(xs) + 1, max(ys) + 1) if xs else None




def shrink_rgba(rows, out_h):
	"""an RGBA image (rows of (r, g, b, a)) box-filtered down to out_h tall, as rows of (coverage 0-1, (r, g, b))"""
	ph, pw = len(rows), len(rows[0])
	out_w = max(1, round(pw * out_h / ph))
	out = []
	for oy in range(out_h):
		sy0, sy1 = oy * ph // out_h, max(oy * ph // out_h + 1, (oy + 1) * ph // out_h)
		row = []
		for ox in range(out_w):
			sx0, sx1 = ox * pw // out_w, max(ox * pw // out_w + 1, (ox + 1) * pw // out_w)
			a = r = g = b = 0.0
			for yy in range(sy0, sy1):
				for xx in range(sx0, sx1):
					pr, pg, pb, pa = rows[yy][xx]
					pa /= 255
					a += pa
					r += pa * pr; g += pa * pg; b += pa * pb
			n = (sy1 - sy0) * (sx1 - sx0)
			row.append((a / n, (r / a, g / a, b / a) if a else (0, 0, 0)))
		out.append(row)
	return out


def draw_logo(out, path, cx, cy, height):
	"""draws a transparent PNG (eg. a logo) centered on (cx, cy), height pixels tall, over out's rows of (r, g, b, a)"""
	_, _, rows = read_png(path)
	logo = shrink_rgba(rows, height)
	ox, oy = int(round(cx - len(logo[0]) / 2)), int(round(cy - height / 2))
	for y, row in enumerate(logo):
		for x, (a, c) in enumerate(row):
			px, py = ox + x, oy + y
			if a <= 0 or not (0 <= px < len(out[0]) and 0 <= py < len(out)):
				continue
			r, g, b, al = out[py][px]
			out[py][px] = (clamp(r + (c[0] - r) * a), clamp(g + (c[1] - g) * a), clamp(b + (c[2] - b) * a), al)
