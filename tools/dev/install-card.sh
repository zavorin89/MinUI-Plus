#!/bin/sh
# Copies the builds from tools/dev/build-a30.sh, the A30 launch script and wifi.sh onto the SD card mounted as
# "MIN UI" (set CARD=... for another name), checking each copy. Leaves the card's .tmp_update alone: it's what the
# A30 boots MinUI+ from, and if it's ever missing the device boots its stock OS (restore it from a release's MinUI.zip)
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
C="${CARD:-/Volumes/MIN UI}"; O="$REPO/tools/dev/out/all"
[ -d "$C/.system" ] || { echo "card not mounted at $C"; exit 1; }
for pair in "minui/build/my282/minui.elf:.system/my282/bin/minui.elf" "minarch/build/my282/minarch.elf:.system/my282/bin/minarch.elf" \
	"rasync/build/my282/rasync.elf:Tools/my282/RetroAchievements.pak/rasync.elf" "wifi/build/my282/wifi.elf:Tools/my282/Wi-Fi.pak/wifi.elf" \
	"boxart/build/my282/boxart.elf:Tools/my282/Box Art.pak/boxart.elf" "controls/build/my282/controls.elf:Tools/my282/Controls.pak/controls.elf"; do
	src="$O/${pair%%:*}"; dst="$C/${pair#*:}"
	[ -d "$(dirname "$dst")" ] || { echo "skipped ${pair#*:} (not on this card)"; continue; }
	cp "$src" "$dst" && cmp "$src" "$dst" && echo "ok ${pair#*:}"
done
for f in "paks/MinUI.pak/launch.sh" "bin/wifi.sh" "bin/update.sh"; do
	cp "$REPO/skeleton/SYSTEM/my282/$f" "$C/.system/my282/$f" && cmp "$REPO/skeleton/SYSTEM/my282/$f" "$C/.system/my282/$f" && echo "ok $f"
done
cp "$O/https/build/my282/https.elf" "$C/.system/my282/bin/https.elf" && \
	cmp "$O/https/build/my282/https.elf" "$C/.system/my282/bin/https.elf" && echo "ok bin/https.elf"
find "$C" -name '._*' -delete 2>/dev/null || true
[ -f "$C/.tmp_update/my282.sh" ] || echo "WARNING: $C/.tmp_update is missing, the A30 will boot its stock OS"
sync
