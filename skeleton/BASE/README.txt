MinUI+ is a fork of MinUI for the Miyoo A30 that brings together the best of MinUI, Onion and spruce: the same simple launcher and in-game menu, plus a box art carousel, RetroAchievements, play time tracking, menu music, borders and screen effects, Wi-Fi, a game switcher, per-game emulator cores, themes, and lots of work on speed and battery life. It comes set up to look its best from the first boot.

MinUI+:
https://gitlab.com/zavorin89-group/MinUI-Plus

MinUI by Shaun Inman:
https://github.com/shauninman/minui

System art from the Nao-Black theme by Naotib (NextUI Themes, MIT license). Credits for all of it are in Roms/.res/CREDITS.txt.

----------------------------------------
Installing

Format a microSD card as FAT32 (MBR).

Copy everything in this zip to the root of the card: the "miyoo" folder, "MinUI.zip" (without unzipping it), and the "Bios", "Roms", "Saves", "BGM", "Emus", "Tools" and "Borders" folders. Put your games in the system folders in "Roms", any bios files in "Bios", and start the A30 with the card in. The first start installs MinUI+.

----------------------------------------
Updating

Copy the new "MinUI.zip" (without unzipping) to the root of the card. Your games, saves and settings are kept.

----------------------------------------
Shortcuts

In a game list:

  L1 / R1: previous / next letter
  L2 / R2: change the sort order
  SELECT: list or carousel view
  Hold SELECT: pin to the game switcher
  START: a random game (in the system list: from any system)
  MENU + Y: game menu (core, box art, rename, delete)
  MENU + X: delete the game

Anywhere:

  MENU: settings, or the pause menu in a game
  Hold MENU: game switcher (quicksaves first in a game)
  MENU + VOLUME: brightness
  POWER: sleep and wake
  Hold POWER: power off

The Controls tool lists these too.

----------------------------------------
Clock

The clock shows beside Wi-Fi and the battery, and Wi-Fi corrects it every time it connects. Change its format (12-hour, 24-hour or off) in the settings' Clock row. The time zone is found automatically the first time, or pick yours in the Time zone row (daylight saving included). The A30 normally keeps time while it's off. If it comes back behind the last time MinUI+ saw (eg. the battery ran out), the clock stays hidden until Wi-Fi syncs it or you set it. No Wi-Fi? Enter the time in the settings' Set time row: the A30 keeps it from there, through power offs.

----------------------------------------
Updates

After Wi-Fi connects, MinUI+ quietly checks its latest GitLab release. When a newer version exists, "Update Available!" appears above the systems. Open it to read the patch notes and confirm the update. A progress bar follows the download and installation. When it is ready, choose Shut Down, then turn the A30 back on to finish, or choose Later to return to MinUI+ and shut down when convenient.

The installed MinUI+ version appears as a subtle final row at the bottom of Settings.

----------------------------------------
Play time

MinUI+ tracks how long you play every game. The pause menu shows the game's total, this session, how many sessions and your average session. Tools > Play Time ranks every game by hours, and shows your total across everything. MENU + X there clears it all (it asks first).

----------------------------------------
Music

Put mp3, ogg, flac or wav files in the "BGM" folder and turn on Music in the settings (tap MENU in the menus). Left and right on that row set its volume. Music plays in the menus and in a game's pause menu, never while you play.

----------------------------------------
Borders and looks

Every system starts with the look that suits it: a border, an LCD grid and LCD ghosting on handhelds, scanlines on TV consoles, color correction for GBC and GBA, and the Game Boy Pocket's greys for Game Boy games. Change any of it in a game's Options > Frontend. Borders live in "Borders", as <TAG>.png (aspect scaling) and <TAG>-native.png (native scaling).

----------------------------------------
RetroAchievements and Wi-Fi

Connect in the settings (tap MENU) > Wi-Fi, then sign in with Tools > RetroAchievements. Achievements then just work in games, offline too: unlocks are sent the next time you're online. The tool also syncs your games' achievements for offline play.

----------------------------------------
Quicksave & auto-resume

MinUI will create a quicksave when powering off in-game. The next time you power on the device it will automatically resume from where you left off. A quicksave is created when powering off manually or automatically after a short sleep. On devices without a POWER button (eg. the Trimui Smart or M17) press the MENU button twice to put the device to sleep before flipping the POWER switch.

----------------------------------------
Roms

Included in this zip is a clean "Roms" folder containing one fully named folder for each console MinUI+ supports (eg. "Game Boy Advance (GBA)"). Put games directly in these descriptive folders; short aliases such as "GBA" are not needed. You can rename a folder, but you must keep the uppercase tag in parentheses so it stays mapped to the correct emulator (eg. "Nintendo Entertainment System (FC)" could be renamed to "NES (FC)").

When one or more folder share the same display name (eg. "Game Boy Advance (GBA)" and "Game Boy Advance (MGBA)") they will be combined into a single menu item containing the roms from both folders (continuing the previous example, "Game Boy Advance"). This allows opening specific roms with an alternate pak.

----------------------------------------
Nintendo 64

Nintendo 64 is experimental, and your mileage may vary. The A30 is at the edge of what it needs, so results depend heavily on the game: expect graphics glitches and slowdowns, and some games won't be playable.

----------------------------------------
Nintendo DS

MinUI+ plays Nintendo DS games with DraStic, which is not part of MinUI+ and is not included in this zip.

1. Download drastic-v1.8_miyoo.zip from https://github.com/steward-fu/nds/releases/tag/v1.8
2. Unzip it into "Emus/my282/NDS.pak" on your SD card, so that you have "Emus/my282/NDS.pak/drastic/drastic"
3. Put your games in "Roms/Nintendo DS (NDS)"

MinUI+ is built for DraStic r2.5.0.4. To check your copy, the SHA-256 of its "drastic" file is:
caecd505c79ebb56701906e64bc9f487d9fcb03c06cb8b113a9165e2ca7c016d

If another copy is used, MinUI+ warns once and still tries to run it. Saves and save states stay in the DraStic folder, so keep that folder when updating.

----------------------------------------
Bios

Some emulators require or perform much better with official bios. MinUI is strictly BYOB. Place the bios for each system in a folder that matches the tag in the corresponding "Roms" folder name (eg. bios for "Sony PlayStation (PS)" roms goes in "/Bios/PS/"),

Bios file names are case-sensitive:

   FC: disksys.rom
   GB: gb_bios.bin
  GBA: gba_bios.bin
  GBC: gbc_bios.bin
   MD: bios_CD_E.bin
       bios_CD_J.bin
       bios_CD_U.bin
   PS: psxonpsp660.bin

----------------------------------------
Disc-based games

To streamline launching multi-file disc-based games with MinUI place your bin/cue (and/or iso/wav files) in a folder with the same name as the cue file. MinUI will automatically launch the cue file instead of navigating into the folder when selected, eg. 

  Harmful Park (English v1.0)/
    Harmful Park (English v1.0).bin
    Harmful Park (English v1.0).cue

Multi-disc games set themselves up: copy every disc into the system's folder, named like "Final Fantasy IX (Disc 1).chd", "Final Fantasy IX (Disc 2).chd" (or "Disk 1", "CD 1"), and the next time the menu opens MinUI+ moves them into a "Final Fantasy IX" folder with an m3u, and carries your memory card and save states over to the merged game.

To set one up by hand instead, put all the files for all the discs in a single folder. Then create an m3u file in that folder (just a text file containing the relative path to each disc's cue file on a separate line) with the same name as the folder. Instead of showing the entire messy contents of the folder, MinUI will launch the appropriate cue file, eg. For a "Policenauts" folder structured like this:

  Policenauts (English v1.0)/
    Policenauts (English v1.0).m3u
    Policenauts (Japan) (Disc 1).bin
    Policenauts (Japan) (Disc 1).cue
    Policenauts (Japan) (Disc 2).bin
    Policenauts (Japan) (Disc 2).cue

The m3u file would contain just:

  Policenauts (Japan) (Disc 1).cue
  Policenauts (Japan) (Disc 2).cue

When a multi-disc game is detected the in-game menu's Continue item will also show the current disc. Press left or right to switch between discs.

MinUI also supports chd files and official pbp files (multi-disc pbp files larger than 2GB are not supported). Regardless of the multi-disc file format used, every disc of the same game share the same memory card and save state slots.

----------------------------------------
Collections

A collection is just a text file containing an ordered list of full paths to rom, cue, or m3u files. These text files live in the "Collections" folder at the root of your SD card, eg. "/Collections/Metroid series.txt" might look like this:

  /Roms/Game Boy Advance (GBA)/Metroid Zero Mission.gba
  /Roms/Game Boy (GB)/Metroid II.gb
  /Roms/Super Nintendo (SFC)/Super Metroid.sfc
  /Roms/Game Boy Advance (GBA)/Metroid Fusion.gba

----------------------------------------

Display names

Certain (unsupported arcade) cores require roms to use arcane file names. You can override the display name used throughout MinUI by creating a map.txt in the same folder as the files you want to rename. One line per file, `rom.ext` followed by a single tab followed by `Display Name`. You can hide a file by adding a `.` at the beginning of the display name. eg.
	
  neogeo.zip	.Neo Geo Bios
  mslug.zip	Metal Slug
  sf2.zip	Street Fighter II

----------------------------------------
Simple mode

Not simple enough for you (or maybe your kids)? MinUI has a simple mode that hides the Tools folder and replaces Options in the in-game menu with Reset. Perfect for handing off to littles (and olds too I guess). Just create an empty file named "enable-simple-mode" (no extension) in "/.userdata/shared/".

----------------------------------------
Advanced

MinUI can automatically run a user-authored shell script on boot. Just place a file named "auto.sh" in "/.userdata/<DEVICE>/". If you're on Windows, make sure your text editor uses Unix line-endings (eg. `\n`), these devices usually choke on Windows line-endings (eg. `\r\n`).

----------------------------------------
Thanks (from MinUI)

To eggs, for his NEON scalers, years of top-notch example code, and patience in the face of my endless questions.

Check out eggs' releases (includes source code): 

  RG35XX https://www.dropbox.com/sh/3av70t99ffdgzk1/AAAKPQ4y0kBtTsO3e_Xlrhqha
  Miyoo Mini https://www.dropbox.com/sh/hqcsr1h1d7f8nr3/AABtSOygIX_e4mio3rkLetWTa
  Trimui Model S https://www.dropbox.com/sh/5e9xwvp672vt8cr/AAAkfmYQeqdAalPiTramOz9Ma

To neonloop, for putting together the original Trimui toolchain from which I learned everything I know about docker and buildroot and is the basis for every toolchain I've put together since, and for picoarch, the inspiration for minarch.

Check out neonloop's repos: 

  https://git.crowdedwood.com

To adixial and acmeplus and the entire muOS community, for sharing their discoveries for the h700 family of Anbernic devices.

Check out muOS and Knulli:

	https://muos.dev
	https://knulli.org

To fewt and the entire JELOS community, for JELOS (without which MinUI would not exist on the RGB30) and for sharing their knowledge with this perpetual Linux kernel novice.

Check out JELOS:

  https://github.com/JustEnoughLinuxOS/distribution

To Steward, for maintaining exhaustive documentation on a plethora of devices:

	https://steward-fu.github.io/website/

To Gamma, for his efforts unlocking more performance on the MagicX XU Mini M (before we all realized its RG3562 was surreptitiously an RK3266).

Check out his repos (including GammaOS):

	https://github.com/TheGammaSqueeze/

To BlackSeraph, for introducing me to chroot.

Check out the GarlicOS repos:

	https://github.com/GarlicOS

To Jim Gray, for commiserating during development, for early alpha testing, and for providing the soundtrack for much of MinUI's development.

Check out Jim's music: 

  https://ourghosts.bandcamp.com/music
  https://www.patreon.com/ourghosts/
 
