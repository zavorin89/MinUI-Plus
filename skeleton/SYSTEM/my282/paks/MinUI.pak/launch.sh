#!/bin/sh

export PLATFORM="my282"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export SAVES_PATH="$SDCARD_PATH/Saves"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime-utc.txt" # in UTC, so a new time zone can't make it look ahead

#######################################

export PATH=$SYSTEM_PATH/bin:$PATH
export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LD_LIBRARY_PATH

#######################################

reclock()
{
	overclock.elf userspace 2 1344 384 1080 0
}

echo 0 > /sys/class/leds/led1/brightness
reclock

# stock firmware services a handheld has no use for, which compete with MinUI for the CPU right after boot:
# a router's DHCP/DNS server, a clock sync (it would replace the time that was set with UTC: there's no time zone) and an empty cron
# (stopped through the service manager, since it restarts dnsmasq and ntpd when they're just killed)
(for SERVICE in dnsmasq sysntpd cron; do /etc/init.d/$SERVICE stop; done) >/dev/null 2>&1 &

# the time zone picked in the settings, for everything started from here (the clock syncs over wifi with it)
TIME_ZONE_PATH="$SHARED_USERDATA_PATH/.minui/time-zone.txt"
[ -f "$TIME_ZONE_PATH" ] && export TZ="$(cat "$TIME_ZONE_PATH")"

# the A30's clock keeps running while it's off. If it's now behind the time last saved, it didn't (eg. the battery ran
# out): the saved time is the best guess, and the clock stays hidden until wifi syncs it or it's set in the settings.
# It also stays hidden until one of those has happened once, or there's no telling if it was ever right. What it said
# is logged, with the last few boots
mkdir -p "$LOGS_PATH"
CLOCK_SYNCED="$SHARED_USERDATA_PATH/.minui/clock-synced"
CLOCK_MANUAL="$SHARED_USERDATA_PATH/.minui/clock-manual" # set by hand with no time zone: shown as it was typed
BOOT_CLOCK=$(date +'%F %T %Z')
RESTORED=no
if [ -f "$DATETIME_PATH" ]; then
	SAVED=$(TZ=UTC0 date -d "$(cat "$DATETIME_PATH")" +%s 2>/dev/null)
	if [ -n "$SAVED" ] && [ "$(date +%s)" -lt "$SAVED" ]; then
		TZ=UTC0 date -s "$(cat "$DATETIME_PATH")" >/dev/null && hwclock --utc -w && RESTORED=yes
		rm -f "$CLOCK_SYNCED" # (otherwise the guess would pass as the real time on the next boot)
	fi
fi
[ "$RESTORED" = no ] && [ -f "$CLOCK_SYNCED" ] && { [ -f "$TIME_ZONE_PATH" ] || [ -f "$CLOCK_MANUAL" ]; } && [ "$(date +%s)" -gt 1735689600 ] && touch /tmp/minui_clock_ok
{
	tail -n 29 "$LOGS_PATH/clock.txt" 2>/dev/null
	echo "boot: the clock said $BOOT_CLOCK, rtc $(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null), last saved $(cat "$DATETIME_PATH" 2>/dev/null) UTC, restored: $RESTORED, shown: $([ -f /tmp/minui_clock_ok ] && echo yes || echo no)"
} > /tmp/minui_clock_log && mv /tmp/minui_clock_log "$LOGS_PATH/clock.txt"

# the LEDs and what they can do (max_brightness 1 is only on or off), for the settings' Sleep light
{ for L in /sys/class/leds/*; do echo "$(basename "$L"): max_brightness $(cat "$L/max_brightness" 2>/dev/null), trigger $(cat "$L/trigger" 2>/dev/null)"; done; } > "$LOGS_PATH/leds.txt" 2>&1

# don't write a file's last-access date to the card every time art or a game is read
mount -o remount,noatime /mnt/SDCARD 2>/dev/null

# Older MinUI cards and some third-party packs created short or obsolete Roms folders alongside MinUI+'s
# descriptive folders (eg. both GBA and Game Boy Advance (GBA)). An empty alias still becomes a duplicate system
# in the menu. rmdir is intentionally used here: a folder containing any game or other file is never touched.
for LEGACY_ROMS in ARCADE FC GB GBA MD MS NEOGEO PCE PS SFC; do
	rmdir "$SDCARD_PATH/Roms/$LEGACY_ROMS" 2>/dev/null || true
done
# Supafaust is available from the main Super Nintendo folder's per-game Core setting; it no longer needs a second
# system row. As above, an existing folder with games is preserved for manual migration.
rmdir "$SDCARD_PATH/Roms/Super Nintendo (SUPA)" 2>/dev/null || true

# The Borders folder is called Bezels now. Move it over; where a manual install already added Bezels, the person's
# Borders copies win, so a bezel they changed or added is kept.
OLD_BEZELS="$SDCARD_PATH/Borders"
if [ -d "$OLD_BEZELS" ]; then
	if [ ! -d "$SDCARD_PATH/Bezels" ]; then
		mv "$OLD_BEZELS" "$SDCARD_PATH/Bezels"
	elif cp -R "$OLD_BEZELS/." "$SDCARD_PATH/Bezels/"; then
		rm -rf "$OLD_BEZELS"
	fi
fi

# MinUI+ only exposes its own focused tools. Older packages also installed six stock Extras that were never part of
# that set. Move them into hidden userdata instead of deleting them, so an existing user can recover one manually.
# Only the once: a tool someone puts on the card afterwards is theirs to keep.
REMOVED_TOOLS_PATH="$SHARED_USERDATA_PATH/.minui/removed-official-tools"
REMOVED_TOOLS_DONE="$SHARED_USERDATA_PATH/.minui/removed-official-tools-done"
if [ ! -f "$REMOVED_TOOLS_DONE" ]; then
mkdir -p "$REMOVED_TOOLS_PATH"
for OFFICIAL_TOOL in "Bootlogo.pak" "Bootlogo.pak.disabled" "Clock.pak" "Files.pak" "Input.pak" "Remove Loading.pak" "Reset Stick.pak"; do
	SOURCE_TOOL="$SDCARD_PATH/Tools/$PLATFORM/$OFFICIAL_TOOL"
	[ -e "$SOURCE_TOOL" ] || continue
	DEST_TOOL="$REMOVED_TOOLS_PATH/$OFFICIAL_TOOL"
	[ -e "$DEST_TOOL" ] && DEST_TOOL="$DEST_TOOL.$(date +%s)"
	mv "$SOURCE_TOOL" "$DEST_TOOL"
done
touch "$REMOVED_TOOLS_DONE"
fi

killall -9 tee
rm -f "$SDCARD_PATH/update.log"
WIFI_FLAG="$SHARED_USERDATA_PATH/.minui/wifi-on" # set by the Wi-Fi setup (Tools/Wi-Fi.pak, opened from the settings)
WIFI_AUTOSTART_OFF="$SHARED_USERDATA_PATH/.minui/wifi-autostart-off" # "Connect at startup: Off" in the Wi-Fi setup
if [ -f "$WIFI_FLAG" ] && [ -f "$WIFI_AUTOSTART_OFF" ]; then
	rm -f "$WIFI_FLAG" # start with wifi off, it's turned back on from the Wi-Fi tool
fi
WIFI_WAS_ON=1 # (so a first pass with wifi off takes it down)
WIFI_WAS_ONLINE=0
CLOCK_TRIES=0
CLOCK_RETRY_AT=0
while :; do
	if [ ! -f "$WIFI_FLAG" ]; then
		[ "$WIFI_WAS_ON" -eq 1 ] && wifi.sh down
		killall -9 wpa_supplicant 2>/dev/null
		WIFI_WAS_ON=0
		WIFI_WAS_ONLINE=0
	elif [ -n "$(wifi.sh status)" ]; then
		# clock and updater on each new connection (including saved networks and hotspots that appear later), and
		# again while the clock hasn't synced: every minute at first, then every 10
		NOW=$(cut -d. -f1 /proc/uptime)
		if [ "$WIFI_WAS_ONLINE" -eq 0 ]; then
			CLOCK_TRIES=0
			CLOCK_RETRY_AT=0
		fi
		if [ "$WIFI_WAS_ONLINE" -eq 0 ] || { [ ! -f /tmp/minui_clock_set ] && [ "$NOW" -ge "$CLOCK_RETRY_AT" ]; }; then
			wifi.sh online >/dev/null 2>&1 &
			CLOCK_TRIES=$((CLOCK_TRIES + 1))
			if [ "$CLOCK_TRIES" -lt 5 ]; then CLOCK_RETRY_AT=$((NOW + 60)); else CLOCK_RETRY_AT=$((NOW + 600)); fi
		fi
		WIFI_WAS_ON=1
		WIFI_WAS_ONLINE=1
	else
		WIFI_WAS_ON=1
		WIFI_WAS_ONLINE=0
	fi
	killall -9 MtpDaemon
	if [ -f /tmp/minui_asleep ]; then sleep 30; else sleep 2; fi # asleep: fewer wakeups
done &
if [ -f "$WIFI_FLAG" ]; then
	wifi.sh up &
fi

#######################################

keymon.elf & #> $LOGS_PATH/keymon.txt 2>&1 &

#######################################

mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"
AUTO_PATH="$USERDATA_PATH/auto.sh"
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH" # > $LOGS_PATH/auto.txt 2>&1
fi

cd $(dirname "$0")

#######################################

# minui's place in the menus: a fresh boot starts at the top (going by uptime, since this script can also be
# started again without a reboot, eg. if /tmp is cleared)
UPTIME=`cut -d. -f1 /proc/uptime`
if [ "$UPTIME" -lt 120 ]; then
	rm -f "$SHARED_USERDATA_PATH/.minui/last.txt"
fi

EXEC_PATH="/tmp/minui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH" && sync
while [ -f "$EXEC_PATH" ]; do
	{ echo "uptime: `cut -d' ' -f1 /proc/uptime`"; ls -a /tmp; df /tmp; } > $LOGS_PATH/tmp.txt 2>&1 # what minui finds in /tmp (its place in the menus lives there)
	minui.elf > $LOGS_PATH/minui.txt 2>&1
	STATUS=$?
	if [ $STATUS -ne 0 ]; then # kept for troubleshooting, minui.txt is replaced on the next run
		{ echo "=== `date +'%F %T'` minui exited with $STATUS"; tail -n 20 $LOGS_PATH/minui.txt; dmesg | tail -n 15; } >> $LOGS_PATH/minui-crash.txt
	fi
	reclock
	echo `TZ=UTC0 date +'%F %T'` > "$DATETIME_PATH"
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		reclock
		echo `TZ=UTC0 date +'%F %T'` > "$DATETIME_PATH"
		sync & # the game's saves reach the card while minui starts, instead of minui waiting on them
	fi
done

shutdown
