#!/bin/sh
# DraStic through libndslayer.so (see workspace/all/ndslayer), so it has the same game menu as every other system.
# DraStic isn't part of MinUI+: people bring their own (see the README). Its release unzips to a drastic folder in this
# pak, which keeps its own launch.sh from replacing this one. A copy directly in this pak or already on the card works too.

PAK="$(cd "$(dirname "$0")" && pwd)"
DRASTIC_SHA256=caecd505c79ebb56701906e64bc9f487d9fcb03c06cb8b113a9165e2ca7c016d # r2.5.0.4, what the layer is built for
DRASTIC=""
for DIR in "$PAK/drastic" "$PAK" "$SYSTEM_PATH/paks/Emus/NDS.pak"; do
	[ -f "$DIR/drastic" ] && DRASTIC="$DIR" && break
done
if [ -z "$DRASTIC" ]; then
	echo "no DraStic: unzip it into $PAK" > "$LOGS_PATH/NDS-$(basename "$1").txt"
	say.elf "DraStic isn't installed

Unzip it into Emus/my282/NDS.pak
The README says where to get it."
	exit 1
fi

MISSING=""
for NAME in config resources system; do
	[ -d "$DRASTIC/$NAME" ] || MISSING="$MISSING $NAME"
done
if [ -n "$MISSING" ]; then
	echo "DraStic in $DRASTIC is missing:$MISSING" > "$LOGS_PATH/NDS-$(basename "$1").txt"
	say.elf "DraStic is missing files:
$MISSING

Unzip the whole release again."
	exit 1
fi

# another build may still run, so this only warns, once for each copy: it doesn't slow every launch
CHECKED="$USERDATA_PATH/NDS-drastic-checked"
STAMP="$DRASTIC $(wc -c < "$DRASTIC/drastic")"
if [ "$(cat "$CHECKED" 2>/dev/null)" != "$STAMP" ]; then
	if command -v sha256sum >/dev/null 2>&1 && [ "$(sha256sum "$DRASTIC/drastic" | sed 's/ .*//')" != "$DRASTIC_SHA256" ]; then
		say.elf "This isn't DraStic r2.5.0.4

MinUI+ is built for that version,
so this copy may not work."
	fi
	echo "$STAMP" > "$CHECKED"
fi

# the layer draws the screens itself and needs them plain
CFG="$DRASTIC/config/drastic.cfg"
if [ -f "$CFG" ]; then
	[ -f "$CFG.stock" ] || cp "$CFG" "$CFG.stock"
	sed -i \
		-e 's/^screen_orientation = .*/screen_orientation = 0/' \
		-e 's/^screen_scaling = .*/screen_scaling = 1/' \
		-e 's/^screen_swap = .*/screen_swap = 0/' \
		-e 's/^hires_3d = .*/hires_3d = 0/' \
		"$CFG"
fi

mkdir -p "$DRASTIC/savestates"

# saves made by an older copy in .system (DraStic used to live there) are copied over once, so that progress isn't
# stranded: in-game saves the copy here doesn't have, and save states, unless this copy has a newer in-game save for
# that game (the state would bring back older progress). Copied with their times, which that check uses. Nothing is
# overwritten or deleted
OLD="$SYSTEM_PATH/paks/Emus/NDS.pak"
COPIED="$USERDATA_PATH/NDS-old-saves-copied"
if [ "$DRASTIC" != "$OLD" ] && [ -d "$OLD" ] && [ ! -f "$COPIED" ]; then
	MOVED_LOG="$LOGS_PATH/NDS-old-saves.txt"
	mkdir -p "$DRASTIC/backup"
	for SAVE in "$OLD"/backup/*.dsv; do
		[ -f "$SAVE" ] || continue
		NAME="$(basename "$SAVE")"
		[ -e "$DRASTIC/backup/$NAME" ] && continue
		cp -p "$SAVE" "$DRASTIC/backup/$NAME" && echo "copied in-game save $NAME" >> "$MOVED_LOG"
	done
	for STATE in "$OLD"/savestates/*.dss; do
		[ -f "$STATE" ] || continue
		NAME="$(basename "$STATE")"
		[ -e "$DRASTIC/savestates/$NAME" ] && continue
		GAME="${NAME%_*.dss}"
		if [ -f "$DRASTIC/backup/$GAME.dsv" ] && [ "$DRASTIC/backup/$GAME.dsv" -nt "$STATE" ]; then
			echo "left $NAME: $GAME has a newer in-game save here" >> "$MOVED_LOG"
			continue
		fi
		cp -p "$STATE" "$DRASTIC/savestates/$NAME" && echo "copied save state $NAME" >> "$MOVED_LOG"
	done
	sync
	touch "$COPIED"
fi

SWAPPINESS=`cat /proc/sys/vm/swappiness`
echo 10 > /proc/sys/vm/swappiness

cd "$DRASTIC"
export HOME="$DRASTIC"
export NDSLAYER_CFG="$USERDATA_PATH/NDS-layer.cfg"
export LD_LIBRARY_PATH="$SYSTEM_PATH/lib:/usr/miyoo/lib:/usr/lib:$PAK/lib" # the stub libasound only if the device has none
LOG="$LOGS_PATH/NDS-$(basename "$1").txt"
LD_PRELOAD="$PAK/libndslayer.so" ./drastic "$1" > "$LOG" 2>&1
STATUS=$?
if [ "$STATUS" -ne 0 ]; then # how it ended, for a crash report: nothing here at all means the device hung
	echo "drastic exited with $STATUS" >> "$LOG"
	dmesg | tail -20 >> "$LOG"
fi
sync

echo $SWAPPINESS > /proc/sys/vm/swappiness
overclock.elf userspace 2 1344 384 1080 0
