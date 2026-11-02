# Screen borders

minarch's "Screen border" option fills the black bars around a game with `Borders/<TAG>.png` (640x480).
The game is drawn on top at its normal size, so only the area outside it shows.

The release ships drkhrse's Miyoo bezels (github.com/drkhrse/drkhrse_miyoo_bezels) in `skeleton/EXTRAS/Borders/`,
credited in its `CREDITS.txt`. The pack has no license file, so they're included with credit and a link.
The steps below are for making borders for systems the pack doesn't cover, in the same style.

## Making them with ChatGPT

Attach two of the shipped borders (eg. `GBC.png` and `GG.png`) as references and use:

> Create a 640x480 image in exactly the same style as the attached references: dark charcoal device-bezel bands with
> subtle texture, a small red power light and grey "POWER" label, and the system's name or wordmark in the console's
> own colors along one band. The area where the game is shown must be pure white (#FFFFFF) and exactly the size and
> position given below. No photos, no extra text.

Then add the system's line below, and save the result as `generated/<TAG>.png`.

| TAG | System | White game area (x, y, width x height) |
| --- | --- | --- |
| GB | Game Boy | 53, 0, 533 x 480 (bands left and right) |
| GBC | Game Boy Color | 53, 0, 533 x 480 |
| GG | Game Gear | 53, 0, 533 x 480 |
| SGB | Super Game Boy | 45, 0, 549 x 480 |
| NGP | Neo Geo Pocket | 67, 0, 505 x 480 |
| NGPC | Neo Geo Pocket Color | 67, 0, 505 x 480 |
| P8 | Pico-8 | 80, 0, 480 x 480 |
| PKM | Pokémon mini | 32, 48, 576 x 384 (scaled 6x, bands on all sides) |
| VB | Virtual Boy | 0, 53, 640 x 373 (bands top and bottom) |
| GBA, MGBA | Game Boy Advance | 0, 26, 640 x 427 (bands top and bottom) |

| CPS1, CPS2, CPS3, FBN, MAME | Arcade | 140, 0, 360 x 480 (a vertical game, bands left and right) |

These are the game's size at each system's default scaling (Aspect, except Pokémon mini which is Native).

The arcade ones aren't made with ChatGPT: `make-arcade-borders.py` draws them (run it with the system-art venv,
which has Pillow). Horizontal arcade games fill the screen; a vertical one is shown full height, and minarch moves the
bands in or out to meet it when it isn't exactly 3:4.

## Installing

Copy `generated/<TAG>.png` to `Borders/<TAG>.png` on the card, and to `skeleton/EXTRAS/Borders/` for the release.
