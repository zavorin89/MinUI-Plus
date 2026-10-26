#!/bin/sh
# Trims full size (4:3, eg. 1024x768) system art the way the card's art is trimmed and installs it:
# scaled to 640x480, then the right-hand 271x480 strip, into Roms/.res/<name>.png
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CARD="${CARD:-/Volumes/MIN UI}"
[ -d "$CARD/Roms" ] || { echo "No card at $CARD (set CARD=...)"; exit 1; }
TMP="$(mktemp -d)"
for SRC in "$HERE"/generated/*.png; do
	[ -f "$SRC" ] || continue
	NAME="$(basename "$SRC")"
	sips -z 480 640 "$SRC" --out "$TMP/full.png" >/dev/null
	sips -c 480 271 --cropOffset 0 369 "$TMP/full.png" --out "$CARD/Roms/.res/$NAME" >/dev/null
	echo "installed $NAME"
done
rm -rf "$TMP"
find "$CARD/Roms/.res" -name '._*' -delete 2>/dev/null || true
sync
