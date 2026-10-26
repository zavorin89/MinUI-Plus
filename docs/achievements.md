# Wi-Fi + RetroAchievements plan (my282 / Miyoo A30)

Status (2026-10-26): everything in the requested order below is built and on the card except the newest step, PlayStation `.chd` hashing, which is built and checked on the Mac but not yet installed or tried on the device.

- Wi-Fi: `Tools/Wi-Fi.pak` + `wifi.sh`, started at boot by `MinUI.pak/launch.sh` when it's on.
- In game (minarch: `ra.c`, `ra_http.c`, `ra_cache.c`, vendored rcheevos): unlock and set-complete popups with a chime, the Achievements list in the pause menu (sortable), badges.
- Offline: sets and badges are cached per game; unlocks made offline are queued (softcore and hardcore kept apart) and sent on the next sync or online launch.
- `Tools/RetroAchievements.pak` (`workspace/all/rasync`): sign in, "Sync all games" (hashes every game on the card, caches its set, sends queued unlocks), and the Hardcore switch.
- Hardcore: a game with achievements starts fresh instead of resuming from its quicksave (in-game saves still work); loading a save state is allowed but ends hardcore for that session, and the Load item warns about it.
- PlayStation: `ra_chd.c` reads `.chd` discs (vendored libchdr) for rcheevos' hashing, both in game and in the Sync tool, and passes `.cue`/`.bin`/`.iso` to rcheevos' own reader. On the Mac a made-up PS disc hashes the same as `.cue`/`.bin`, as `.chd` and through a multi-disc `.m3u`. The Sync tool now also finds multi-disc folders (`<folder>/<folder>.m3u`) and skips `.bin` files that belong to a `.cue`.

## Probe results (2026-10-23)

- Present: `/usr/sbin/wpa_supplicant`, `/usr/sbin/wpa_cli`, `/sbin/udhcpc`, `/usr/sbin/iw`, `/usr/sbin/iwconfig`, `/sbin/ip`, `/usr/bin/wget`, `/usr/sbin/ntpd`. Missing: `curl`, `wpa_passphrase`, `ntpdate`.
- `wlan0` exists (down, because of MinUI's kill loop); rfkill `phy0` is unblocked. The driver is built in (no wifi `.ko` modules).
- Stock config and scripts: `/etc/wifi/wpa_supplicant.conf` (`ctrl_interface=DIR=/var/run/wpa_supplicant`), `/etc/wifi/udhcpc_wlan0`, `/sbin/wifi`, `/lib/libwifimg.so`.
- TLS: the firmware ships OpenSSL 1.1 (`libssl.so.1.1`, `libcrypto.so.1.1`), so step 2 can dynamically link those instead of building mbedTLS. Compile against OpenSSL 1.1 headers; link against the device's libraries (copy them off the device with a pak, or build matching 1.1.x for the toolchain sysroot).
- No CA certificates on the device (`/etc/ssl` is empty): ship a CA bundle on the card and point the TLS context at it.
- Kernel 3.4.39, armv7l.

## What exists today

- `skeleton/SYSTEM/my282/paks/MinUI.pak/launch.sh` kills `wpa_supplicant` and runs `ifconfig wlan0 down` every 2 seconds, forever.
- `workspace/my282/platform/platform.c` has the online check commented out (`/sys/class/net/wlan0/operstate`, see `PLAT_getBatteryStatus`); `workspace/tg5040/platform/platform.c` has a working version. The Wi-Fi status icon already exists (`ASSET_WIFI`, drawn when `PLAT_isOnline()`).
- The my282 toolchain (Buildroot 2017.11) has no OpenSSL, mbedTLS, or libcurl.
- minarch already tracks per-game play time (`play` struct in `workspace/all/minarch/minarch.c`) and the in-game menu has a stats panel; achievements UI can follow the same patterns.

## 1. Wi-Fi

- Credentials: `.userdata/shared/wifi.txt` (`ssid=` / `psk=` lines), edited on a computer since MinUI has no on-screen keyboard.
- MENU info screen row "Wi-Fi: Off / On" (same row pattern as the power timers in `minui.c`), stored next to `POWER_PATH`.
- `launch.sh`: only run the kill loop when Wi-Fi is off; when on, start `wpa_supplicant -B -i wlan0 -c <generated conf>` and `udhcpc -i wlan0 -b` (exact binaries/paths per the probe report). Set the clock with `ntpd`/`ntpdate` if present.
- Un-comment the my282 online check so the Wi-Fi icon shows.

## 2. RetroAchievements in minarch (softcore only)

- Vendor rcheevos (MIT, `rc_client` API) under `workspace/all/minarch/rcheevos/`.
- HTTPS: the firmware's OpenSSL 1.1 plus a minimal HTTP/1.1 client (no curl on the device). Needs a CA bundle on the card.
- Login: `.userdata/shared/retroachievements.txt` with username + password once; exchange for a token, then rewrite the file without the password.
- Hook `rc_client_do_frame()` once per emulated frame in the main loop (next to `Play_tick()`), memory via the core's `retro_get_memory_data` / memory maps.
- UI: unlock toast drawn over the game (my282 `PLAT_flip`, like the border), and an "Achievements" item in the in-game menu (`Menu_loop`).
- Hardcore was first left out (MinUI's quicksave/auto-resume relies on save states); it was added later as an option, see step 3 of the requested order.
- Systems: everything minarch runs that rcheevos knows, including PlayStation `.chd` (see Status). NDS runs outside minarch (no support).

## 3. Offline achievements

- Cache each game's achievement set (and badge images) under `.userdata/shared/.achievements/<hash>/` whenever it's fetched online.
- Two ways to fill the cache: automatically when a game is launched with Wi-Fi on, and a "Sync achievements" action that hashes every ROM on the card and downloads all sets (cartridge systems are quick; PS discs are slow).
- Offline: evaluate from the cache, show unlocks immediately, append them to a queue file, submit the queue next time online.
- Optional Mac route: a script run with the card mounted that pre-caches sets and submits the queue, so achievements work with device Wi-Fi off. Check how spruceOS handles its offline achievements before finalizing the format.

## Build/test notes

- Device builds: `docker run -i my282-toolchain`, piping in `workspace/my282`, `workspace/all/common` and the program folders (`minui`, `minarch`, `rasync`, `wifi`, `boxart`), since the external drive isn't shared with Colima. minarch's makefile clones `libretro-common` itself.
- The A30 is a Cortex-A7 (ARMv7): check every build has no ARMv8-only instructions (`vmaxnm`, `vminnm`, `vrint*`, `vsel*`, `vcvt[anpm]`) with `objdump -d`.
- Mac check for disc hashing: build a small program from `ra_chd.c`, `libchdr/unity.c` and rcheevos' `hash`, `util` and `runtime` sources, and compare a disc's hash as `.cue`/`.bin` and as `.chd` (`chdman createcd`, from Homebrew `rom-tools`).
- Networking can't be tested on the macOS build; every step needs an on-device test.

## Requested order (2026-10-23)

1. Done. In-game achievements list (menu item, title/points/description/progress, locked vs unlocked).
2. Done. Offline sync:
   - Automatic per game: when a game is launched with Wi-Fi on and its achievement set isn't cached yet, fetch and cache it (skip if already cached).
   - "Sync all games" option that caches every game on the card in one go, for fully offline play.
   - Offline unlocks queue to a file and are submitted the next time Wi-Fi is on.
3. Done. Optional hardcore: in-game (battery) saves are fine in hardcore; save states, rewind, slow motion, and cheats are not. A hardcore option would have to disable menu Save/Load, X-to-resume, and the sleep/power-off quicksave. RetroAchievements only accepts hardcore unlocks from approved clients, so test whether the server accepts this build before relying on it.
4. Built, not yet tried on the device. PlayStation (.chd) last: hook rcheevos' cdreader up to libchdr (or the core's disc access) for hashing.
