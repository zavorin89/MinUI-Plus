#!/usr/bin/env python3
"""Sets MinUI+'s out-of-the-box look in each system's default.cfg: aspect scaling, the system's border, an LCD grid
on handhelds and scanlines on TV consoles, the core's LCD ghosting where it has one, and the Auto CPU speed.
Only these keys are touched; everything else in a default.cfg (bindings, core options) is kept.

    python3 tools/set-defaults.py <folder with *.pak/default.cfg> [more folders...]
"""
import os
import sys

LOOK = {"minarch_screen_scaling": "Aspect", "minarch_screen_sharpness": "Crisp", "minarch_cpu_speed": "Auto"}
LCD = dict(LOOK, minarch_screen_effect="Grid", minarch_screen_border="On")
TV = dict(LOOK, minarch_screen_effect="Line")

SYSTEMS = {
	# handhelds: their LCD's grid, a border, and the LCD's slow response
	# the Game Boy as a Pocket showed it: near black and white (the grid takes the palette's tint)
	"GB": dict(LCD, gambatte_mix_frames="lcd_ghosting", gambatte_gb_colorization="internal", gambatte_gb_internal_palette="GB - Pocket"),
	# colors as the dim original screens showed them, not the oversaturated raw values
	"GBC": dict(LCD, gambatte_mix_frames="lcd_ghosting", gambatte_gbc_color_correction="GBC only", gambatte_gbc_color_correction_mode="accurate"),
	"GBA": dict(LCD, gpsp_frame_mixing="enabled", gpsp_color_correction="enabled"),
	"MGBA": dict(LCD, mgba_interframe_blending="lcd_ghosting", mgba_color_correction="Auto"),
	"GG": dict(LCD, picodrive_ggghost="normal"),
	"NGP": dict(LCD),
	"NGPC": dict(LCD),
	# the core draws its own dot matrix (at 3x), a grid over it would clash; "analog" is its LCD ghosting
	"PKM": dict(LOOK, minarch_screen_effect="None", minarch_screen_border="On", pokemini_lcdfilter="dotmatrix", pokemini_lcdmode="analog"),
	# no LCD grid to imitate
	"VB": dict(LOOK, minarch_screen_effect="None", minarch_screen_border="On"),
	"P8": dict(LOOK, minarch_screen_effect="None", minarch_screen_border="On"),
	# played on a TV
	"SGB": dict(TV, minarch_screen_border="On"),
	"FC": dict(TV), "SFC": dict(TV), "SFC10": dict(TV), "SNESX": dict(TV), "SUPA": dict(TV),
	"MD": dict(TV), "SMS": dict(TV), "PCE": dict(TV), "PS": dict(TV),
}


def apply(path, settings):
	lines = open(path, encoding="utf-8").read().split("\n") if os.path.exists(path) else []
	done = set()
	out = []
	for line in lines:
		key = line.split("=", 1)[0].strip().lstrip("-") if "=" in line and not line.startswith("bind ") else None
		if key in settings:
			if key in done:
				continue  # a duplicate
			locked = line.startswith("-")
			out.append(f"{'-' if locked else ''}{key} = {settings[key]}")
			done.add(key)
		else:
			out.append(line)
	missing = [f"{k} = {v}" for k, v in settings.items() if k not in done]
	if missing:
		at = next((i for i, l in enumerate(out) if l.startswith("bind ")), len(out))
		while at > 0 and out[at - 1] == "":
			at -= 1
		block = missing + ([""] if at < len(out) and out[at] != "" else [])
		out[at:at] = block
	tidy = []
	for line in out:
		if line=="" and tidy and tidy[-1]=="":
			continue  # one blank line between groups is enough
		tidy.append(line)
	text = "\n".join(tidy)
	if not text.endswith("\n"):
		text += "\n"
	with open(path, "w", encoding="utf-8") as f:
		f.write(text)


def main():
	if len(sys.argv) < 2:
		print(__doc__)
		sys.exit(1)
	for folder in sys.argv[1:]:
		for pak in sorted(os.listdir(folder)):
			tag = pak[:-4] if pak.endswith(".pak") else None
			if tag not in SYSTEMS:
				continue
			path = os.path.join(folder, pak, "default.cfg")
			apply(path, SYSTEMS[tag])
			print(f"{tag}: {path}")


main()
