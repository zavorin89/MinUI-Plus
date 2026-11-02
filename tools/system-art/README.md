# System art

The pictures MinUI shows beside the main menu for each system, in the style of the Nao-Black theme:
a real photo of the console on black, angled slightly, overlapping a solid colored panel with a slanted edge on the right.

The theme's originals are 1024x768. On the A30 they're shown as the right-hand 271x480 strip of the image scaled
to 640x480, so **keep the console in the right half** (roughly x 560-950 of 1024) and vertically centered.

## Making the missing ones with ChatGPT

Attach two or three images from `reference/` and use the prompt for the system, then save the result as
`generated/<folder name>.png` (the exact names are below).

Shared prompt (put first):

> Create a 1024x768 image in exactly the same style as the attached references: a pure black background, a single
> photorealistic product shot of the console floating slightly tilted, placed in the right half of the image and
> vertically centered, overlapping a solid flat-colored vertical panel on the far right whose left edge is a slight
> diagonal (like the references). No text, no logos added, no shadows on the background, no other objects.

Per system:

| Save as | Add to the prompt |
| --- | --- |
| `Virtual Boy (VB).png` | The console is a Nintendo Virtual Boy (red and black, on its stand). Panel color: deep red, #8c1414. |
| `Pokemon mini (PKM).png` | The console is a Pokémon mini handheld (small, rounded, wolf blue). Panel color: soft blue, #3f6fb0. |
| `Pico-8 (P8).png` | Instead of a console, a small retro handheld with a screen showing the PICO-8 boot logo colors. Panel color: PICO-8 pink, #ff77a8. |
| `Super Game Boy (SGB).png` | The console is a Super Game Boy cartridge plugged into a Super Nintendo. Panel color: lavender grey, #8e8bb0. |

## Installing

With the SD card mounted as `MIN UI`:

```sh
tools/system-art/install.sh
```

It scales each image in `generated/` to 640x480, keeps the right-hand 271x480 strip (exactly how the existing
art on the card was trimmed) and copies it to `Roms/.res/` on the card.

## Compositing supplied cutouts

`compose-system-art.py` makes both the editable 1024x768 source and the exact 271x480 A30 strip from a transparent
hardware cutout. It uses the same crop as `install.sh`; specify maximum subject width and height to keep differently
shaped consoles and cabinets at a consistent visual size.

The CPS1, CPS2, CPS3, Final Burn Neo, MAME 2003+, Neo Geo, Sega CD and Sega 32X sources were composed with this
tool. CPS1 and MAME first needed background extraction; the remaining supplied images already had transparency.
