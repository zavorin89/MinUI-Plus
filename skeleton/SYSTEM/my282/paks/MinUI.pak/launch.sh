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
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

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

# the time as it was last saved, if the clock is behind it: the A30's clock stops while it's off, so this is the
# best guess until wifi syncs it (the clock stays hidden until then). What the clock said is logged, to check that
mkdir -p "$LOGS_PATH"
BOOT_CLOCK=$(date +'%F %T')
RESTORED=no
if [ -f "$DATETIME_PATH" ]; then
	SAVED=$(date -d "$(cat "$DATETIME_PATH")" +%s 2>/dev/null)
	if [ -n "$SAVED" ] && [ "$(date +%s)" -lt "$SAVED" ]; then
		date -s "$(cat "$DATETIME_PATH")" >/dev/null && hwclock --utc -w && RESTORED=yes
	fi
fi
echo "boot: the clock said $BOOT_CLOCK, last saved $(cat "$DATETIME_PATH" 2>/dev/null), restored: $RESTORED" > "$LOGS_PATH/clock.txt"

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

killall -9 tee
rm -f "$SDCARD_PATH/update.log"
WIFI_FLAG="$SHARED_USERDATA_PATH/.minui/wifi-on" # set by the Wi-Fi setup (Tools/Wi-Fi.pak, opened from the settings)
WIFI_AUTOSTART_OFF="$SHARED_USERDATA_PATH/.minui/wifi-autostart-off" # "Connect at startup: Off" in the Wi-Fi setup
if [ -f "$WIFI_FLAG" ] && [ -f "$WIFI_AUTOSTART_OFF" ]; then
	rm -f "$WIFI_FLAG" # start with wifi off, it's turned back on from the Wi-Fi tool
fi
WIFI_WAS_ONLINE=0
while :; do
	if [ ! -f "$WIFI_FLAG" ]; then
		killall -9 wpa_supplicant
		ifconfig wlan0 down
		WIFI_WAS_ONLINE=0
	elif [ -n "$(wifi.sh status)" ]; then
		if [ "$WIFI_WAS_ONLINE" -eq 0 ]; then
			wifi.sh online >/dev/null 2>&1 & # clock and updater, including saved networks/hotspots that appear later
		fi
		WIFI_WAS_ONLINE=1
	else
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
	echo `date +'%F %T'` > "$DATETIME_PATH"
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		reclock
		echo `date +'%F %T'` > "$DATETIME_PATH"
		sync & # the game's saves reach the card while minui starts, instead of minui waiting on them
	fi
done

shutdown
