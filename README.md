# MinUI+

**MinUI+ is a fork of [MinUI](https://github.com/shauninman/MinUI) for the Miyoo A30.** It keeps what makes MinUI
great: one simple launcher, the same quick in-game menu everywhere, instant sleep and resume. It adds what you'd
miss coming from other handheld firmware: a box art carousel, RetroAchievements, menu music, borders and screen
effects, Wi-Fi, a game switcher, per-game emulator cores, and a lot of work on speed and battery life.

It's preconfigured to look its best from the first boot: every system has its border, LCD grid or scanlines,
LCD ghosting and color correction already set up, and the CPU speed adjusts itself to each game.

> MinUI+ is made for and tested on the **Miyoo A30** only.

- [Highlights](#highlights)
- [Installing](#installing)
- [The launcher](#the-launcher)
- [In game](#in-game)
- [Systems and cores](#systems-and-cores)
- [Tools](#tools)
- [Speed and battery](#speed-and-battery)
- [Controls](#controls)
- [Folders on the card](#folders-on-the-card)
- [Building](#building)
- [Credits](#credits)

## Highlights

- **Box art carousel**, plus box art beside the list, with art downloaded right on the device
- **RetroAchievements**: unlock popups, an achievements list in the pause menu, hardcore mode, and playing offline
  (unlocks are sent later), PlayStation `.chd` discs included
- **Game menu** (MENU + Y on a game): pick its emulator core, download its box art, rename it, delete it
- **Game switcher**: hold MENU to jump between pinned games, each picking up right where you left off
- **Background music** in the menus and the pause menu, from your own mp3, ogg, flac or wav files
- **Borders** for every handheld system, at aspect and at native scaling, in each console's own colors
- **Preconfigured looks**: LCD grid and ghosting on handhelds, scanlines on TV consoles, Game Boy Pocket greys,
  color correction for GBC and GBA
- **Auto CPU speed**: each game runs at the lowest speed that keeps it smooth, and remembers what it needed
- **Wi-Fi** with an on-screen keyboard, offered automatically when a tool needs it
- **Fast and smooth**: art decodes on its own thread, menus are GPU drawn, saving and the game switcher are quick
- **Efficient sleep**: the CPU drops to a crawl, audio and Wi-Fi switch off, the device lasts far longer asleep

## Installing

MinUI+ installs like MinUI:

1. Format a microSD card as **FAT32**.
2. Unzip `MinUIPlus-<date>-base.zip` onto it. That's the launcher, the base systems, and the `Bios`, `Roms`,
   `Saves` and `BGM` folders.
3. Optionally, unzip `MinUIPlus-<date>-extras.zip` onto it too: more systems (Game Gear, Neo Geo Pocket, PC
   Engine, Pokémon mini, Virtual Boy, Pico-8, Master System, Super Game Boy, mGBA, Supafaust), the borders, and the
   tools (Wi-Fi, Box Art, RetroAchievements, Controls and more).
4. Put your games in the system folders in `Roms`, any BIOS files your systems need in `Bios`, and insert the card.

To update, unzip the new base zip over the card. Your games, saves and settings are kept.

## The launcher

### Views

- **Carousel**: the selected game's box art big in the middle, its neighbors either side, sliding as you scroll.
  SELECT switches between the carousel and the list.
- **List with art**: the classic MinUI list, with the selected game's box art beside it. The art stays up while you
  scroll, so the list doesn't jump.
- **Systems**: the system list, with system art.

### Finding games

- **L1 / R1** jump to the previous or next letter, with a big letter popup showing where you are.
- **L2 / R2** change the sort order: A to Z, Z to A, recently played, or favorites first.
- **START** picks a random game.
- **Favorites** (Y on a game), **Recently Played**, and **Most Played** with play time for each game.

### The game menu (MENU + Y)

- **Change core**: some systems have more than one emulator. Pick one per game, with a line about what each is good
  at; the system's default stays on top. Saves and states are kept per core, so switching never loses progress.
- **Get box art** / **Replace box art**: downloads this game's cover from the libretro thumbnail library (turns
  Wi-Fi on if you'd like). Covers are resized to 250 pixels wide so they load fast.
- **Rename**: edit the game's name on the on-screen keyboard. Its box art, saves, save states, settings, border,
  play time, favorite, recents and collections all follow the new name. Multi-disc games too.
- **Delete game**: asks first, then whether to also delete its saves, and its box art.

MENU + X deletes a game directly (still asking first).

### Game switcher (hold MENU)

Hold MENU anywhere, in the menus or in a game, to open the game switcher: your pinned games, each with a picture
of where you left it. A picks a game up from that exact moment. In a game, holding MENU quicksaves first, with a
"Saving" spinner if it takes a moment. Pin games with a long press of SELECT in a list, or X in the pause menu.

### Settings (tap MENU)

- **Play time** in total, and **Most played** (A opens the full list, X twice clears it)
- **Sleep after** and **Power off after** timers
- **Theme**: several color themes for the whole interface
- **Menu sounds**: subtle clicks for moving, opening and going back
- **Music**: background music on or off, and its volume (Left/Right). Put songs in the `BGM` folder

### Music

Background music plays in the launcher and in a game's pause menu, never while you play. Put **mp3, ogg, flac or
wav** files in `BGM` at the top of the card and turn on Music in the settings. Songs play shuffled, each once
before any repeats, fading in and out. In the pause menu the song picks up where it left off between visits. It's
decoded a little ahead on a lowest-priority thread, so it never slows the menus: about 2% of the CPU.

### Look and feel

- Volume, brightness and every popup fade out smoothly
- MinUI keeps its place in the menus, even when you quit a game from its pause menu
- MENU dismisses an achievement popup instead of opening the pause menu

## In game

### Pause menu

The MinUI pause menu, quicker to open, with:

- **Save / Load** with a picture of each save state slot as the game looked, border and grid included
- **Achievements** (with RetroAchievements set up): every achievement with its badge, locked or unlocked, sortable
- **Options**: emulator options, controls and shortcuts, now with clicks for moving and changing values
- **Music** from the `BGM` folder, if it's on
- **X** pins the game to the game switcher

### Screen

- **Borders**: each handheld has a border at aspect scaling, and at native scaling a full console face in its shell
  color (silver Game Boy Pocket, Kiwi lime Game Boy Color, indigo Game Boy Advance, black Game Gear, Neo Geo Pocket
  and Color) with a dark screen lens, a glowing power light and the console's logo. Turn them on or off per system
  in Options > Frontend > Screen Border. Add your own as `Borders/<SYSTEM>.png` (or `<SYSTEM>-native.png`), or for one
  game as `Borders/<SYSTEM>/<game file>.png`.
- **Effects**: an LCD grid, or scanlines, at every scaling.
- **LCD ghosting**: handhelds blend frames like their slow original screens did.
- **Color correction**: GBC and GBA colors as their dim original screens showed them, instead of oversaturated.
- **Game Boy Pocket look**: original Game Boy games in the Pocket's near black-and-white greys.

### RetroAchievements

Sign in with the RetroAchievements tool (on the on-screen keyboard), then just play:

- Unlock popups with the achievement's badge and a chime, and a summary when a game starts
- Games with no achievements, or that RetroAchievements doesn't know, show nothing at all
- **Hardcore** mode (no save states, no fast forward), switched in the tool
- **Offline**: unlocks earned without Wi-Fi are kept and sent the next time you're online. The tool syncs your games
  ahead of time so their achievements and badges work offline
- PlayStation `.chd` games are identified too

## Systems and cores

| System | Folder tag | Default core | Other cores (pick per game) |
| --- | --- | --- | --- |
| Game Boy | GB | gambatte | |
| Game Boy Color | GBC | gambatte | |
| Game Boy Advance | GBA | gpSP | mGBA (extras) |
| NES | FC | FCEUmm | |
| Super Nintendo | SFC | snes9x 2005 | snes9x 2010, snes9x (current), Supafaust (extras) |
| Genesis | MD | PicoDrive | |
| PlayStation | PS | PCSX ReARMed | |
| Game Gear, Master System | GG, SMS | PicoDrive (extras) | |
| Neo Geo Pocket / Color | NGP, NGPC | RACE (extras) | |
| PC Engine | PCE | Beetle PCE Fast (extras) | |
| Pokémon mini | PKM | PokeMini (extras) | |
| Virtual Boy | VB | Beetle VB (extras) | |
| Pico-8 | P8 | fake-08 (extras) | |
| Super Game Boy | SGB | mGBA (extras) | |

Each system starts with the look that suits it:

| | Scaling | Effect | Border | Ghosting | Colors |
| --- | --- | --- | --- | --- | --- |
| Game Boy | Aspect | LCD grid | Game Boy Pocket | LCD ghosting | Pocket greys |
| Game Boy Color | Aspect | LCD grid | Game Boy Color | LCD ghosting | Color correction |
| Game Boy Advance | Aspect | LCD grid | Game Boy Advance | Frame blending (mGBA: LCD ghosting) | Color correction |
| Game Gear | Aspect | LCD grid | Game Gear | Normal ghosting | |
| Neo Geo Pocket / Color | Aspect | LCD grid | Neo Geo Pocket | | |
| Pokémon mini | Aspect | its own dot matrix | Pokémon mini | Analog LCD | |
| Virtual Boy, Pico-8 | Aspect | | yes | | |
| Super Game Boy | Aspect | Scanlines | Super Game Boy | | |
| NES, SNES, Genesis, Master System, PC Engine, PlayStation | Aspect | Scanlines | | | |

Everything can be changed per system or per game in the pause menu's Options, like in MinUI.

## Tools

- **Wi-Fi**: scan, pick a network, type its password on the keyboard, connect. Choose whether it connects at
  startup. Tools that need the internet offer to turn it on.
- **RetroAchievements**: sign in, switch hardcore on or off, and sync your games' achievements and badges for
  offline play.
- **Box Art**: downloads covers for every game missing one from the libretro thumbnail library, or resizes the ones
  you have.
- **Controls**: a short reference of the shortcuts you can't see on screen.

## Speed and battery

- **Smooth menus**: box art is decoded on a background thread ahead of where you're scrolling, menus are drawn by
  the GPU through a ring of textures so frames never wait on it, and the CPU gets a short boost while you navigate.
- **Quick everything**: the pause menu opens without waiting, saves write their picture at half size as a 16-bit
  image and flush the card in the background, and the game switcher starts without waiting on the card.
- **Auto CPU speed** (the default for every system): each frame, minarch measures how much of its time went to
  emulating it, and runs the CPU at the lowest of 480 to 1512 MHz that keeps every frame comfortably on time. It
  steps up the moment a frame runs late, and remembers each game's needs so it starts at the right speed next time.
  Game Boy games run at a fraction of the CPU; heavy PlayStation games get all of it. Powersave, Normal and
  Performance are still there to pick by hand.
- **Sleep**: one core at 240 MHz, the audio device closed, Wi-Fi off after a minute asleep (back on when you wake),
  and the background tasks quiet.
- **Audio**: smoother resampling, and the A30's output at 48000 Hz.
- **At boot**: stock firmware services a handheld has no use for are stopped, and the card isn't written to every
  time a file is read.

## Controls

Everything on screen has a button hint. These are the ones you won't see:

| In a game list | |
| --- | --- |
| L1 / R1 | Previous / next letter |
| L2 / R2 | Sort order |
| SELECT | List or carousel |
| Hold SELECT | Pin to the game switcher |
| START | Random game |
| MENU + Y | Game menu: core, box art, rename, delete |
| MENU + X | Delete the game |

| Everywhere | |
| --- | --- |
| MENU | Settings, or the pause menu in a game |
| MENU (with an achievement popup up) | Hide the popup |
| Hold MENU | Game switcher (quicksaves first in a game) |
| MENU + VOL | Brightness |
| Hold POWER | Power off (quicksaves first in a game) |

| On the keyboard | |
| --- | --- |
| X | Shift, for one capital letter |
| Y | Numbers and symbols |
| L1 / R1 | Move the cursor |
| SELECT | Cancel |
| START | Done |

## Folders on the card

| Folder | What goes there |
| --- | --- |
| `Roms/<System (TAG)>` | Games, eg. `Roms/Game Boy Advance (GBA)` |
| `Roms/<System>/.res` | Box art, `<game file>.png` (the Box Art tool fills it) |
| `Roms/.res` | System art, `<System folder>.png` |
| `Bios/<TAG>` | BIOS files |
| `Saves/<TAG>` | In-game saves |
| `BGM` | Music for the menus: mp3, ogg, flac or wav |
| `Borders` | Screen borders, `<TAG>.png` and `<TAG>-native.png` |
| `Tools` | Wi-Fi, RetroAchievements, Box Art, Controls and more |
| `.userdata` | Settings, save states, achievements, play time, logs (`.userdata/my282/logs`) |

## Building

MinUI+ builds like MinUI, in Docker, on macOS or Linux:

```sh
make
```

builds the A30 release into `releases/` (`MinUIPlus-<date>-base.zip` and `-extras.zip`). The first build also
builds the emulator cores from the libretro sources, which takes a while.

Some extra pieces live in `tools/`:

- `tools/set-defaults.py` writes the per-system looks into each system's `default.cfg`
- `tools/borders/make-native-borders.py` and `make-gba-logo.py` generate the native borders and the GBA logo
- `tools/merge-core-folders.py` moves games from a core's own system folder (eg. an `SFC10` folder) into the main
  one, keeping each on its core and moving its saves, states and art along

## Credits

- **[MinUI](https://github.com/shauninman/MinUI)** by Shaun Inman: everything MinUI+ is built on.
- **Screen borders** by **drkhrse**, from the [drkhrse Miyoo bezels](https://github.com/drkhrse/drkhrse_miyoo_bezels).
  The native borders are made for MinUI+ around their logos and power lights.
- **Emulator cores** from **[libretro](https://github.com/libretro)** and their authors: gambatte, gpSP, mGBA,
  FCEUmm, snes9x 2005 / 2010 / current, Supafaust, PicoDrive, PCSX ReARMed, RACE, Beetle PCE Fast, Beetle VB,
  PokeMini and fake-08. Each is under its own license; snes9x's allows non-commercial use only.
- **[rcheevos](https://github.com/RetroAchievements/rcheevos)** for RetroAchievements, and
  [RetroAchievements.org](https://retroachievements.org).
- **[libchdr](https://github.com/rtissera/libchdr)** for reading `.chd` discs.
- **[dr_libs](https://github.com/mackron/dr_libs)** (dr_mp3, dr_wav, dr_flac) and
  **[stb_vorbis](https://github.com/nothings/stb)** for music, both public domain.
- Box art from the **[libretro thumbnails](https://github.com/libretro-thumbnails/libretro-thumbnails)** library.
- Game Boy, Game Boy Color, Game Boy Advance and Nintendo are trademarks of Nintendo. SEGA and Game Gear are
  trademarks of SEGA. Neo Geo Pocket is a trademark of SNK. MinUI+ isn't affiliated with any of them.

MinUI's original README is in [docs/upstream-README.md](docs/upstream-README.md).
