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
ARCADE = dict(TV, minarch_screen_border="On")
FBNEO = {"fbneo-vertical-mode": "disabled", "fbneo-hiscores": "enabled", "fbneo-allow-depth-32": "disabled",
	"fbneo-cyclone": "enabled", "fbneo-samplerate": "44100", "fbneo-sample-interpolation": "disabled",
	"fbneo-fm-interpolation": "disabled", "fbneo-frameskip-type": "disabled"}

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
	"SEGACD": dict(TV), "THIRTYTWOX": dict(TV),
	# drawn by the GPU (MinArch's hwrender.c) at the A30's own 320x240, with the lightest plugin and the ARM dynarec;
	# the A30 has no second stick, so the C buttons are the alternate map's (L1/R1, X/A), Z on L2 and R on R2
	"N64": dict(TV, **{"parallel-n64-cpucore": "dynamic_recompiler",
		"parallel-n64-gfxplugin": "rice", "parallel-n64-gfxplugin-accuracy": "low", "parallel-n64-screensize": "320x240",
		"parallel-n64-rspplugin": "auto", "parallel-n64-alt-map": "enabled", "parallel-n64-pak1": "memory"}),
	# arcade, on its CRT with a bezel. Rotation is left to MinArch (its Vertical Games option: upright or sideways),
	# high scores are kept, and the sound is set for the A30's CPU (44.1 kHz, no interpolation)
	"CPS1": dict(ARCADE, fba2012cps1_auto_rotate="enabled", fba2012cps1_hiscores="enabled",
		fba2012cps1_lowpass_filter="disabled", fba2012cps1_frameskip="disabled"),
	"CPS2": dict(ARCADE, fba2012cps2_auto_rotate="enabled", fba2012cps2_hiscores="enabled",
		fba2012cps2_lowpass_filter="disabled", fba2012cps2_frameskip="disabled"),
	"NEOGEO": dict(ARCADE),
	# FBNeo: 16-bit colour (MinArch's), the fast ARM 68000 core, no frame skipping
	"CPS3": dict(ARCADE, **FBNEO), "FBN": dict(ARCADE, **FBNEO),
	"MAME": dict(ARCADE, **{"mame2003-plus_skip_disclaimer": "enabled", "mame2003-plus_skip_warnings": "enabled",
		"mame2003-plus_tate_mode": "disabled", "mame2003-plus_frameskip": "0", "mame2003-plus_display_artwork": "disabled",
		"mame2003-plus_sample_rate": "44100"}),
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
