#!/bin/sh
# The official PICO-8, for anyone who has bought it: MinUI+ doesn't include it (its license is personal). Unzip the
# Raspberry Pi download from lexaloffle.com into this pak, so it holds pico-8/pico8_dyn and pico-8/pico8.dat (see the
# README). Carts go in Roms/Pico-8 Native (PICO8), or pick it for one Pico-8 (P8) cart from MENU + Y. Splore.splore
# there (an empty file) opens Splore, PICO-8's own cart browser, which needs Wi-Fi (see bin/wget). fake-08 keeps
# running Pico-8 (P8) as before.
#
# libp8layer.so (workspace/all/p8layer) makes it play like the other systems: MENU opens a game menu (screen effect,
# scaling, border, buttons, quit), START is PICO-8's pause menu, A/X are O and B/Y are X, the power button sleeps, and
# the volume is the launcher's.

PAK="$(cd "$(dirname "$0")" && pwd)"
ROM="$1"
rm -f /tmp/resume_slot.txt # (picked from the game switcher: PICO-8 has no save states, the switcher only shows its picture)
LOG="$LOGS_PATH/PICO8-$(basename "$ROM").txt"


PICO8=""
for DIR in "$PAK/pico-8" "$PAK/pico8" "$PAK"; do
	[ -f "$DIR/pico8_dyn" ] && PICO8="$DIR" && break
done
if [ -z "$PICO8" ]; then
	echo "no PICO-8: unzip the Raspberry Pi release into $PAK" > "$LOG"
	say.elf "PICO-8 isn't installed

Unzip the Raspberry Pi version
into Emus/my282/PICO8.pak
(the README has the details)"
	exit 1
fi
if [ ! -f "$PICO8/pico8.dat" ]; then
	echo "PICO-8 in $PICO8 has no pico8.dat" > "$LOG"
	say.elf "PICO-8 is missing pico8.dat

Unzip the whole Raspberry Pi
release again."
	exit 1
fi

# its settings and the carts' own saves (cartdata) live with the other saves
P8_HOME="$SAVES_PATH/PICO8"
mkdir -p "$P8_HOME"
CONFIG="$P8_HOME/config.txt"
if [ -f "$CONFIG" ]; then
	# the layer turns the picture and sends PICO-8's own keys, so none of its screen turns or custom keys
	sed -i \
		-e "s/^transform_screen .*/transform_screen 0/" \
		-e "/^button_keys /d" \
		-e "s/^opengl_blit .*/opengl_blit 0/" \
		-e "s/^use_wget .*/use_wget 1/" \
		"$CONFIG"
	grep -q '^use_wget ' "$CONFIG" || echo "use_wget 1" >> "$CONFIG"
else
	printf 'transform_screen 0\nuse_wget 1\n' > "$CONFIG" # (PICO-8 adds the rest)
fi

ROOT="$(dirname "$ROM")" # Splore's local carts: the folder this was started from
if [ "${ROM##*.}" = "splore" ] && [ -z "$(wifi.sh status 2>/dev/null)" ]; then
	say.elf "Splore needs Wi-Fi to browse new carts

Turn it on from the settings.
Carts already downloaded still play." # (and then on: PICO-8 opens Splore either way)
fi
overclock.elf userspace 2 1512 384 1080 0
cd "$PICO8"
export HOME="$P8_HOME"
export LD_LIBRARY_PATH="$SYSTEM_PATH/lib:/usr/miyoo/lib:/usr/lib"
export PATH="$PAK/bin:$PATH" # Splore's downloads (use_wget 1) go through bin/wget: the A30's own wget can't do HTTPS
export P8LAYER_ROM="$ROM" # for play time and the menu's title
export LD_PRELOAD="$PAK/libp8layer.so" # (it also keeps PICO-8 from asking the A30's SDL2 for sensors, which it hasn't got)
case "$ROM" in
	*.splore) ./pico8_dyn -home "$P8_HOME" -root_path "$ROOT" -splore > "$LOG" 2>&1 ;;
	*) ./pico8_dyn -home "$P8_HOME" -root_path "$ROOT" -run "$ROM" > "$LOG" 2>&1 ;;
esac
STATUS=$?
if [ "$STATUS" -ne 0 ]; then # how it ended, for a crash report
	echo "pico8_dyn exited with $STATUS" >> "$LOG"
	dmesg | tail -20 >> "$LOG"
fi
sync
overclock.elf userspace 2 1344 384 1080 0
