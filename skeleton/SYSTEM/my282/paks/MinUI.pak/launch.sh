#!/bin/sh

# boot timing: seconds since power-on at each step, kept in /tmp and saved to logs/boot.txt once the menu is up
stamp() { read UPTIME _ < /proc/uptime; echo "$UPTIME $1" >> /tmp/minui_boot.txt; }
stamp "launch.sh"

# the firmware pipes this script's output through tee into update.log, and tee is killed (in the background, below):
# anything written to that pipe afterwards would end the script (SIGPIPE), and the firmware would start it over.
# Nothing here needs it. (Each process started costs the boot ~10-20 ms, so what can be a shell builtin is one.)
exec >/dev/null 2>&1

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

# The menu's libraries (SDL2 and what it uses, about 2.5 MB) are in the firmware's squashfs, which reads at about
# 5 MB/s as it decompresses; the card reads at about 19 MB/s. So the card keeps a copy, made once from this A30's own
# firmware (never part of a release), used only while it still matches the firmware's files. The GPU driver isn't
# copied: it's loaded under three names, and with a copy as well it could end up loaded twice.
LIB_CACHE="$USERDATA_PATH/lib-cache"
LIB_SOURCES="/usr/miyoo/lib/libSDL2-2.0.so.0 /usr/lib/libasound.so.2 /usr/lib/libudev.so.1 /usr/miyoo/lib/libSDL2_image-2.0.so.0 /usr/lib/libpng12.so.0 /usr/lib/libz.so.1 /usr/miyoo/lib/libSDL2_ttf-2.0.so.0 /usr/lib/libfreetype.so.6 /usr/lib/libbz2.so.1.0"
LIB_STAMP=$(ls -lL $LIB_SOURCES 2>&1)
set -- $LIB_STAMP; LIB_STAMP="$*" # one line (without a process), so it can be compared with read
LIB_CACHED=0
LIB_SAVED=""
[ -f "$LIB_CACHE/stamp.txt" ] && read -r LIB_SAVED < "$LIB_CACHE/stamp.txt"
if [ -n "$LIB_SAVED" ] && [ "$LIB_SAVED" = "$LIB_STAMP" ]; then
	LIB_CACHED=1
	export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LIB_CACHE:$LD_LIBRARY_PATH
else
	export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LD_LIBRARY_PATH
	# made after this boot's menu is up, for the next one
	(
		sleep 15
		rm -rf "$LIB_CACHE.new"
		mkdir -p "$LIB_CACHE.new" || exit
		for LIB in $LIB_SOURCES; do cp "$LIB" "$LIB_CACHE.new/${LIB##*/}" || exit; done
		echo "$LIB_STAMP" > "$LIB_CACHE.new/stamp.txt" && rm -rf "$LIB_CACHE" && mv "$LIB_CACHE.new" "$LIB_CACHE" && sync
	) &
fi

# read into memory now, on cores that are otherwise idle, so the menu doesn't wait on them as it starts. The firmware's
# squashfs decompresses one file at a time, so its files go in the order the menu needs them: what it links to before
# it starts (unless the card has them), then the GPU driver SDL loads for the window. The card's are read beside them.
if [ $LIB_CACHED = 1 ]; then
	{ cat /usr/lib/libMali.so > /dev/null 2>&1; stamp "GPU driver read"; } &
	{ cat "$LIB_CACHE"/lib* > /dev/null 2>&1; stamp "libraries read from the card"; } &
else
	{ cat $LIB_SOURCES /usr/lib/libMali.so > /dev/null 2>&1; stamp "libraries and GPU driver read"; } &
fi
cat "$SYSTEM_PATH/bin/minui.elf" "$SDCARD_PATH/.system/res/BPreplayBold-unhinted.otf" "$SDCARD_PATH/.system/res/assets@2x.png" > /dev/null 2>&1 &

#######################################

# what isn't needed to draw the menu (wifi, the clock check, the rest of the housekeeping) starts as soon as the menu's
# first frame is up (minui touches /tmp/minui_menu_up), so it doesn't share the card and CPU with the menu's start.
# Run in the background; it gives up waiting after about 5 s (eg. if the menu couldn't start)
afterMenu() {
	WAITED=0
	while [ ! -f /tmp/minui_menu_up ] && [ $WAITED -lt 50 ]; do
		usleep 100000 2>/dev/null || sleep 1
		WAITED=$((WAITED + 1))
	done
	return 0
}

# the A30 boots with four cores and MinUI+ runs on two. Switching one off takes ~0.15-0.2 s of waiting, so it's done in
# the background, beside whatever is starting, and only for a core that's still on (shell builtins only: no process)
coresOff() {
	for CORE in 2 3; do
		read -r ON < /sys/devices/system/cpu/cpu$CORE/online 2>/dev/null
		[ "$ON" = 1 ] && echo 0 > /sys/devices/system/cpu/cpu$CORE/online
	done
}

# The boot does only what the menu needs before it can start: everything else runs beside it, in the background.
echo 0 > /sys/class/leds/led1/brightness
# the governor, GPU and RAM clocks and the cores, a moment after the menu starts. The CPU's speed is left as it is: the
# menu sets its own, and when the boot resumes a game, this lands while it's running (and reset its speed before)
(sleep 3; overclock.elf userspace 0 0 384 1080 0; coresOff) &

# stock firmware services a handheld has no use for, which compete with MinUI for the CPU right after boot:
# a router's DHCP/DNS server, a clock sync (it would replace the time that was set with UTC: there's no time zone), an
# empty cron, MTP (file transfer over USB, unused) and adbd (USB debugging: a root shell for anyone with a cable, and
# memory held for nothing). Stopped through the service manager, since it restarts dnsmasq and ntpd when they're just
# killed, and once more a little later in case the firmware was still starting one (it starts MTP last). The USB port
# itself is left as it is, so charging from a computer is unchanged
(
	afterMenu
	for SERVICE in dnsmasq sysntpd cron mtp adbd; do /etc/init.d/$SERVICE stop; done
	sleep 20
	killall -9 MtpDaemon adbd
	# once (until the file is deleted): what the kernel offers for saving power asleep, only read, nothing changed.
	# Suspend states, the radio switches, the USB devices (the Wi-Fi chip is one), the Wi-Fi driver's power settings,
	# the CPU's speeds and the power rails
	PROBE="$LOGS_PATH/power-probe.txt"
	[ -f "$PROBE" ] || {
		echo "== /sys/power"; ls /sys/power
		for F in /sys/power/state /sys/power/mem_sleep /sys/power/autosleep /sys/power/wake_lock /sys/power/scene_lock; do
			[ -e $F ] && echo "$F: $(cat $F)"
		done
		echo "== rfkill"
		for D in /sys/class/rfkill/*; do [ -d "$D" ] && echo "$D: name=$(cat $D/name) type=$(cat $D/type) state=$(cat $D/state) soft=$(cat $D/soft)"; done
		echo "== usb"
		for D in /sys/bus/usb/devices/*; do
			[ -f "$D/idVendor" ] || continue
			echo "$D: $(cat $D/idVendor):$(cat $D/idProduct) $(cat $D/product) control=$(cat $D/power/control) autosuspend=$(cat $D/power/autosuspend_delay_ms)"
			for I in "$D"/*:*; do [ -e "$I/driver" ] && echo "  $(basename $I) driver=$(basename $(readlink $I/driver))"; done
		done
		echo "== wifi driver"
		for D in /sys/module/*/parameters; do
			case "$D" in *rtl*|*rtw*|*8188*|*8189*|*8723*|*wlan*) echo "$D:"; for F in "$D"/*power* "$D"/*ips* "$D"/*lps* "$D"/*usb*; do [ -f "$F" ] && echo "  $(ls -l $F | cut -c1-10) $(basename $F)=$(cat $F)"; done ;; esac
		done
		echo "== cpufreq"
		for F in /sys/devices/system/cpu/cpu0/cpufreq/*; do [ -f "$F" ] && echo "$(basename $F): $(cat $F)"; done
		echo "== regulators"
		for R in /sys/class/regulator/*; do echo "$(basename $R): $(cat $R/name) $(cat $R/microvolts) $(cat $R/state)"; done
	} > "$PROBE" 2>&1
) >/dev/null 2>&1 &

# the time zone picked in the settings, for everything started from here (the clock syncs over wifi with it)
TIME_ZONE_PATH="$SHARED_USERDATA_PATH/.minui/time-zone.txt"
# (read also fills TZ from a file with no newline at the end, it just says it hit the end first)
if [ -f "$TIME_ZONE_PATH" ]; then read -r TZ < "$TIME_ZONE_PATH"; export TZ; fi

[ -d "$LOGS_PATH" ] && [ -d "$SHARED_USERDATA_PATH/.minui" ] || mkdir -p "$LOGS_PATH" "$SHARED_USERDATA_PATH/.minui"

# Older MinUI cards and some third-party packs created short or obsolete Roms folders alongside MinUI+'s
# descriptive folders (eg. both GBA and Game Boy Advance (GBA)). An empty alias still becomes a duplicate system
# in the menu, so this is done before it starts (rmdir only for the ones there). rmdir is intentionally used here: a
# folder containing any game or other file is never touched. Supafaust is available from the main Super Nintendo
# folder's per-game Core setting; it no longer needs a second system row.
ROMS="$SDCARD_PATH/Roms"
for LEGACY_ROMS in ARCADE FC GB GBA MD MS NEOGEO PCE PS SFC "Super Nintendo (SUPA)"; do
	[ -d "$ROMS/$LEGACY_ROMS" ] && rmdir "$ROMS/$LEGACY_ROMS"
done

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

# the rest of the housekeeping, once the menu is up
(
	afterMenu
	stamp "housekeeping and wifi begin"
	# don't write a file's last-access date to the card every time art or a game is read
	mount -o remount,noatime /mnt/SDCARD 2>/dev/null

	# the A30's clock keeps running while it's off. If it's now behind the time last saved, it didn't (eg. the battery
	# ran out): the saved time is the best guess, and the clock stays hidden until wifi syncs it or it's set in the
	# settings. It also stays hidden until one of those has happened once, or there's no telling if it was ever right.
	# What it said is logged, with the last few boots. (The menu looks for /tmp/minui_clock_ok as it runs.)
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

	# how long the power button is held to turn the A30 on (the power chip's setting: 128 ms, 1 s, 2 s or 3 s). It reads
	# 3 s at boot (set by the firmware), so it's written each time, for the next power on; a drained battery resets it.
	# 128 ms, to try it out: only ever shorter, so it can't undo Boot Tweaks' shorter hold
	PEK=/sys/devices/platform/axp22_board
	read -r PEK_BEFORE < $PEK/axp22_pekopen 2>/dev/null
	[ -n "$PEK_BEFORE" ] && [ "$PEK_BEFORE" != 128 ] && echo 128 > $PEK/axp22_pekopen 2>/dev/null
	read -r PEK_AFTER < $PEK/axp22_pekopen 2>/dev/null
	echo "power-on hold: was '$PEK_BEFORE', now '$PEK_AFTER' (long press '$(cat $PEK/axp22_peklong 2>/dev/null)', power cut '$(cat $PEK/axp22_pekclose 2>/dev/null)')" > "$LOGS_PATH/power-key.txt" 2>&1

	# MinUI+ only exposes its own focused tools. Older packages also installed six stock Extras that were never part
	# of that set. Move them into hidden userdata instead of deleting them, so an existing user can recover one
	# manually. Only the once: a tool someone puts on the card afterwards is theirs to keep.
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
) &
stamp "housekeeping started"

WIFI_FLAG="$SHARED_USERDATA_PATH/.minui/wifi-on" # set by the Wi-Fi setup (Tools/Wi-Fi.pak, opened from the settings)
WIFI_AUTOSTART_OFF="$SHARED_USERDATA_PATH/.minui/wifi-autostart-off" # "Connect at startup: Off" in the Wi-Fi setup
if [ -f "$WIFI_FLAG" ] && [ -f "$WIFI_AUTOSTART_OFF" ]; then
	rm -f "$WIFI_FLAG" # start with wifi off, it's turned back on from the Wi-Fi tool
fi
WIFI_WAS_ON=1 # (so a first pass with wifi off takes it down)
WIFI_WAS_ONLINE=0
WIFI_KILLS=0 # passes left that make sure wpa_supplicant stays gone after wifi goes off (the firmware may still be starting it)
CLOCK_TRIES=0
CLOCK_RETRY_AT=0
LOOP_STARTED=0
PASS=0
# Every pass costs as few processes as it can (each one wakes the CPU for a while): the link state is read with a
# builtin, and wifi.sh (a shell with ip, sed and head under it) only runs to see whether a link that just came up has
# its address, and every half minute after to see it still does. It used to run, or two killalls did, every 2 s.
while :; do
	[ $LOOP_STARTED = 0 ] && afterMenu && LOOP_STARTED=1 # (the first pass once the menu is up; this whole loop runs in the background)
	if [ -f /tmp/minui_asleep ]; then sleep 30; continue; fi # asleep: nothing to look after, and as few wakeups as can be
	if [ ! -f "$WIFI_FLAG" ]; then
		if [ "$WIFI_WAS_ON" -eq 1 ]; then
			wifi.sh down
			WIFI_KILLS=6
		fi
		if [ $WIFI_KILLS -gt 0 ]; then
			killall -9 wpa_supplicant 2>/dev/null
			WIFI_KILLS=$((WIFI_KILLS - 1))
		fi
		WIFI_WAS_ON=0
		WIFI_WAS_ONLINE=0
	else
		LINK=""
		read -r LINK < /sys/class/net/wlan0/operstate 2>/dev/null
		# on but not running (eg. it couldn't start after a sleep): started again every two minutes. Once it runs it
		# joins whichever saved network is in range by itself, and moves to another when that one goes
		if [ "$LINK" != up ] && [ $((PASS % 24)) = 12 ] && ! pidof wpa_supplicant >/dev/null; then
			wifi.sh up >/dev/null 2>&1 &
		fi
		ONLINE=0
		if [ "$LINK" = up ]; then
			if [ "$WIFI_WAS_ONLINE" -eq 1 ] && [ $((PASS % 6)) != 0 ]; then ONLINE=1 # (still up: still online)
			elif [ -n "$(wifi.sh status)" ]; then ONLINE=1
			fi
		fi
		if [ $ONLINE = 1 ]; then
			# clock and updater on each new connection (including saved networks and hotspots that appear later), and
			# again while the clock hasn't synced: every minute at first, then every 10
			read -r NOW _ < /proc/uptime
			NOW=${NOW%.*}
			if [ "$WIFI_WAS_ONLINE" -eq 0 ]; then
				CLOCK_TRIES=0
				CLOCK_RETRY_AT=0
			fi
			if [ "$WIFI_WAS_ONLINE" -eq 0 ] || { [ ! -f /tmp/minui_clock_set ] && [ "$NOW" -ge "$CLOCK_RETRY_AT" ]; }; then
				wifi.sh online >/dev/null 2>&1 &
				CLOCK_TRIES=$((CLOCK_TRIES + 1))
				if [ "$CLOCK_TRIES" -lt 5 ]; then CLOCK_RETRY_AT=$((NOW + 60)); else CLOCK_RETRY_AT=$((NOW + 600)); fi
			fi
			WIFI_WAS_ONLINE=1
		else
			WIFI_WAS_ONLINE=0
		fi
		WIFI_WAS_ON=1
	fi
	PASS=$((PASS + 1))
	sleep 5
done &
if [ -f "$WIFI_FLAG" ]; then
	(afterMenu; wifi.sh up) &
fi

#######################################

stamp "wifi started"
keymon.elf & #> $LOGS_PATH/keymon.txt 2>&1 &
stamp "keymon started"

#######################################

AUTO_PATH="$USERDATA_PATH/auto.sh"
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH" # > $LOGS_PATH/auto.txt 2>&1
fi

cd "${0%/*}"

#######################################

# minui's place in the menus: a fresh boot starts at the top (going by uptime, since this script can also be
# started again without a reboot, eg. if /tmp is cleared)
read UPTIME _ < /proc/uptime
if [ "${UPTIME%.*}" -lt 120 ] && [ -f "$SHARED_USERDATA_PATH/.minui/last.txt" ]; then
	rm -f "$SHARED_USERDATA_PATH/.minui/last.txt"
fi

# the boot's timings (each step, in seconds since power-on) are saved in logs/boot.txt once the menu is up
if [ ! -f /tmp/minui_boot_saved ]; then
	: > /tmp/minui_boot_saved
	(sleep 15; cp /tmp/minui_boot.txt "$LOGS_PATH/boot.txt") & # (late enough to include a resumed game's start)
fi
stamp "starting minui"

EXEC_PATH="/tmp/minui_exec"
NEXT_PATH="/tmp/next"
: > "$EXEC_PATH" # (/tmp is in memory: nothing to sync)
while [ -f "$EXEC_PATH" ]; do
	[ -f /tmp/minui_tmp_logged ] || { : > /tmp/minui_tmp_logged; { echo "uptime: `cut -d' ' -f1 /proc/uptime`"; ls -a /tmp; df /tmp; } > $LOGS_PATH/tmp.txt 2>&1 & } # what minui finds in /tmp at boot (its place in the menus lives there): not again on every return from a game
	stamp "minui run"
	minui.elf > $LOGS_PATH/minui.txt 2>&1
	STATUS=$?
	stamp "minui exited ($STATUS)"
	if [ $STATUS -ne 0 ]; then # kept for troubleshooting, minui.txt is replaced on the next run
		{ echo "=== `date +'%F %T'` minui exited with $STATUS"; tail -n 20 $LOGS_PATH/minui.txt; dmesg | tail -n 15; } >> $LOGS_PATH/minui-crash.txt
		# the newest crashes only, so a run of them can't fill the card
		[ "$(wc -c < $LOGS_PATH/minui-crash.txt)" -gt 65536 ] && tail -n 600 $LOGS_PATH/minui-crash.txt > /tmp/minui-crash.txt && mv /tmp/minui-crash.txt $LOGS_PATH/minui-crash.txt
	fi
	# (no overclock.elf here: every launch follows a button press, which already has the CPU at 1344 MHz, minarch and
	# the paks set their own as they start, and nothing changes the GPU or RAM clocks set at boot)
	coresOff & # (at boot, a resumed game starts while the two spare cores go off, instead of after)
	{ TZ=UTC0 date +'%F %T' > "$DATETIME_PATH"; sync; } & # the time, and minui's few writes, reach the card while the game starts

	if [ -f $NEXT_PATH ]; then
		read -r CMD < $NEXT_PATH # (a builtin, where cat started a process before every game: minui writes one line)
		eval $CMD
		rm -f $NEXT_PATH
		{ TZ=UTC0 date +'%F %T' > "$DATETIME_PATH"; sync; } & # the game's saves reach the card while minui starts, instead of minui waiting on them
	fi
done

shutdown
# poweroff returns while the system is still going down: without this, the firmware would start this script again
# and the menu could come back up for a moment (with the battery already reading 0)
while :; do sleep 60; done
