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
# a router's DHCP/DNS server, a clock sync that retries forever offline (wifi.sh sets the clock itself) and an empty cron
# (stopped through the service manager, since it restarts dnsmasq and ntpd when they're just killed)
(for SERVICE in dnsmasq sysntpd cron; do /etc/init.d/$SERVICE stop; done) >/dev/null 2>&1 &

# don't write a file's last-access date to the card every time art or a game is read
mount -o remount,noatime /mnt/SDCARD 2>/dev/null

killall -9 tee
rm -f "$SDCARD_PATH/update.log"
WIFI_FLAG="$SHARED_USERDATA_PATH/.minui/wifi-on" # set by Tools/Wi-Fi.pak
WIFI_AUTOSTART_OFF="$SHARED_USERDATA_PATH/.minui/wifi-autostart-off" # "Connect at startup: Off" in Tools/Wi-Fi.pak
if [ -f "$WIFI_FLAG" ] && [ -f "$WIFI_AUTOSTART_OFF" ]; then
	rm -f "$WIFI_FLAG" # start with wifi off, it's turned back on from the Wi-Fi tool
fi
while :; do
	if [ ! -f "$WIFI_FLAG" ]; then
		killall -9 wpa_supplicant
		ifconfig wlan0 down
	fi
	killall -9 MtpDaemon
	sleep 2
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
		sync
	fi
done

shutdown
