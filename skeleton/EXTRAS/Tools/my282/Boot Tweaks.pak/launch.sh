#!/bin/sh
# Boot Tweaks (optional): changes four small pieces of the A30's own boot loader (u-boot, in its internal flash), for:
# - the LED and a short buzz as soon as the A30 starts (instead of when Linux is up)
# - a short press of power is enough to start it, and plugging in USB no longer starts it
# - a quieter, slightly faster kernel start
# It only runs when picked here, asks first, and only touches flash that matches Miyoo's firmware 20240713 (stock, or
# already changed by this tool). changes.txt holds only our own bytes, never Miyoo's. The original pieces are saved
# to the card first, so Undo can put them back. Made in the separate A30-firmware project.
cd "$(dirname "$0")"
CHANGES="$PWD/changes.txt"
MTD=/dev/mtd0
BACKUP="${USERDATA_PATH:-/mnt/SDCARD/.userdata/my282}/boot-tweaks-backup"
LOG="${LOGS_PATH:-/mnt/SDCARD/.userdata/my282/logs}/boot-tweaks.txt"
TMP=/tmp/boot-tweaks
FIRMWARE_PAGE="lomiyoo.com/pages/firmware"
mkdir -p "$TMP" "$(dirname "$LOG")"
log() { echo "$(date +'%F %T') $*" >> "$LOG"; }

ask() { say.elf --ask "$1" "$2" "$3"; } # B = $1, A = $2; returns 0 for A
tell() { say.elf "$1"; }

sectors() { grep '^sector ' "$CHANGES"; }
read_sector() { dd if=$MTD bs=4096 skip=$(($1 / 4096)) count=1 of="$2" 2>/dev/null; }
md5() { md5sum "$1" | cut -d' ' -f1; }

# what each piece is now: stock, patched or unknown
STATE_STOCK=0; STATE_PATCHED=0; STATE_UNKNOWN=0
while read -r _ S STOCK PATCHED WHAT; do
	read_sector "$S" "$TMP/now-$S.bin"
	case "$(md5 "$TMP/now-$S.bin")" in
		"$STOCK") STATE_STOCK=$((STATE_STOCK + 1)); echo stock > "$TMP/state-$S" ;;
		"$PATCHED") STATE_PATCHED=$((STATE_PATCHED + 1)); echo patched > "$TMP/state-$S" ;;
		*) STATE_UNKNOWN=$((STATE_UNKNOWN + 1)); echo unknown > "$TMP/state-$S"; log "sector $S ($WHAT) is unknown: $(md5 "$TMP/now-$S.bin")" ;;
	esac
done <<EOF
$(sectors)
EOF
log "checked: $STATE_STOCK stock, $STATE_PATCHED changed, $STATE_UNKNOWN unknown"

if [ "$STATE_UNKNOWN" -gt 0 ]; then
	tell "Nothing was changed: this A30's
boot loader isn't one Boot Tweaks knows.
It needs Miyoo's firmware 20240713
($FIRMWARE_PAGE)."
	exit 0
fi

battery_ok() {
	[ "$(cat /sys/class/power_supply/usb/online 2>/dev/null)" = 1 ] && return 0
	[ "$(cat /sys/class/power_supply/battery/capacity 2>/dev/null || echo 0)" -ge 20 ]
}

# writes one 4 KB piece and checks it; $1 offset, $2 file, $3 expected md5
write_sector() {
	for TRY in 1 2; do
		flash_erase $MTD "$1" 1 >/dev/null 2>&1 &&
		dd if="$2" of=$MTD bs=4096 seek=$(($1 / 4096)) count=1 conv=notrunc 2>/dev/null
		sync
		read_sector "$1" "$TMP/check.bin"
		[ "$(md5 "$TMP/check.bin")" = "$3" ] && { log "wrote sector $1 (try $TRY)"; return 0; }
		log "sector $1 didn't verify on try $TRY: $(md5 "$TMP/check.bin")"
	done
	return 1
}

failed() {
	log "FAILED at sector $1"
	tell "Writing failed. Don't turn it off:
open Boot Tweaks again to retry.
(logs/boot-tweaks.txt)"
	exit 1
}

if [ "$STATE_STOCK" -gt 0 ]; then
	ask "CANCEL" "CONTINUE" "Tweak the A30 bootloader.
- Increases boot speed.
- Shorter Power hold to turn on.
- A vibration when powering on.
Backed up and can be undone here.
Stays on in other launchers too." || exit 0
	battery_ok || { tell "Charge to at least 20% first
(or plug it in), then try again."; exit 0; }
	ask "CANCEL" "WRITE" "Don't turn the A30 off while it
writes (a few seconds). The original
pieces are saved to the card first." || exit 0

	mkdir -p "$BACKUP"
	while read -r _ S STOCK PATCHED WHAT; do
		[ "$(cat "$TMP/state-$S")" = stock ] || continue
		cp "$TMP/now-$S.bin" "$BACKUP/sector-$S.bin" && sync
		[ "$(md5 "$BACKUP/sector-$S.bin")" = "$STOCK" ] || failed "$S (backup)"
		cp "$TMP/now-$S.bin" "$TMP/new-$S.bin"
		grep "^byte $S " "$CHANGES" | while read -r _ _ OFF OCT; do
			printf "\\$OCT" | dd of="$TMP/new-$S.bin" bs=1 seek="$OFF" count=1 conv=notrunc 2>/dev/null
		done
		[ "$(md5 "$TMP/new-$S.bin")" = "$PATCHED" ] || failed "$S (build)"
		write_sector "$S" "$TMP/new-$S.bin" "$PATCHED" || failed "$S"
	done <<EOF
$(sectors)
EOF
	log "applied"
	tell "Done. The changes take effect
from the next power on."
	exit 0
fi

# everything is already changed: offer Undo
if ! ls "$BACKUP"/sector-*.bin >/dev/null 2>&1; then
	tell "Boot Tweaks is on. With no saved
copy to undo it, reinstalling Miyoo's
firmware 20240713 removes it
($FIRMWARE_PAGE)."
	exit 0
fi
ask "CLOSE" "UNDO" "Revert all changes to
stock boot settings." || exit 0
battery_ok || { tell "Charge to at least 20% first
(or plug it in), then try again."; exit 0; }

# every saved copy must be there and match stock before anything is written
sectors | while read -r _ S STOCK PATCHED WHAT; do
	[ -f "$BACKUP/sector-$S.bin" ] && [ "$(md5 "$BACKUP/sector-$S.bin")" = "$STOCK" ] || { log "no good backup for $S"; exit 2; }
done || { tell "The saved copy is missing or damaged,
so nothing was undone."; exit 0; }

# undo in reverse order, each from the copy saved before
sectors | sed -n '1!G;h;$p' | while read -r _ S STOCK PATCHED WHAT; do
	write_sector "$S" "$BACKUP/sector-$S.bin" "$STOCK" || exit 1
done
[ $? = 0 ] || failed undo
log "undone"
tell "Undone. The A30's original boot
loader is back from the next power on."
