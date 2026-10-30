#!/bin/sh
# wifi.sh up|down|status|online|time
# online: runs the quiet tasks that need a newly working connection (clock sync and update check)
# Connects wlan0 using the network in /mnt/SDCARD/.userdata/shared/wifi.txt:
#   ssid=My Network
#   psk=my password        (leave out for open networks)
# status prints the IPv4 address, or nothing when not connected.

CREDS=/mnt/SDCARD/.userdata/shared/wifi.txt
CONF=/tmp/wpa_supplicant.conf
DHCP_SCRIPT=/tmp/udhcpc.sh
RUNTIME_PATH="${MINUI_RUNTIME_PATH:-/tmp}"
ONLINE_LOCK="$RUNTIME_PATH/minui_wifi_online.lock"

ip_address() {
	ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1
}

# a time zone name (eg. America/New_York) as a TZ rule, the same ones the settings list
zone_rule() {
	case "$1" in
		Pacific/Honolulu) echo "HST10" ;;
		America/Anchorage|America/Juneau|America/Nome|America/Sitka|America/Yakutat) echo "AKST9AKDT,M3.2.0,M11.1.0" ;;
		America/Los_Angeles|America/Vancouver|America/Tijuana) echo "PST8PDT,M3.2.0,M11.1.0" ;;
		America/Phoenix) echo "MST7" ;;
		America/Denver|America/Boise|America/Edmonton|America/Ciudad_Juarez) echo "MST7MDT,M3.2.0,M11.1.0" ;;
		America/Chicago|America/Winnipeg|America/Menominee|America/Indiana/Knox|America/Indiana/Tell_City|America/North_Dakota/*) echo "CST6CDT,M3.2.0,M11.1.0" ;;
		America/New_York|America/Detroit|America/Toronto|America/Indiana/*|America/Kentucky/*|America/Nassau) echo "EST5EDT,M3.2.0,M11.1.0" ;;
		America/Halifax|America/Moncton|America/Glace_Bay|Atlantic/Bermuda) echo "AST4ADT,M3.2.0,M11.1.0" ;;
		America/Sao_Paulo|America/Argentina/*|America/Montevideo) echo "<-03>3" ;;
		UTC|Etc/UTC|Etc/GMT) echo "UTC0" ;;
		Europe/London|Europe/Dublin|Europe/Lisbon|Atlantic/Canary|Atlantic/Madeira|Atlantic/Faroe) echo "GMT0BST,M3.5.0/1,M10.5.0" ;;
		Europe/Athens|Europe/Helsinki|Europe/Kiev|Europe/Kyiv|Europe/Bucharest|Europe/Sofia|Europe/Riga|Europe/Tallinn|Europe/Vilnius|Europe/Chisinau|Asia/Nicosia) echo "EET-2EEST,M3.5.0/3,M10.5.0/4" ;;
		Europe/Moscow|Europe/Minsk|Europe/Simferopol) echo "MSK-3" ;;
		Europe/*|Africa/Ceuta|Arctic/Longyearbyen) echo "CET-1CEST,M3.5.0,M10.5.0/3" ;;
		Asia/Kolkata|Asia/Calcutta) echo "IST-5:30" ;;
		Asia/Shanghai|Asia/Chongqing|Asia/Hong_Kong|Asia/Macau|Asia/Taipei) echo "CST-8" ;;
		Asia/Tokyo) echo "JST-9" ;;
		Asia/Seoul) echo "KST-9" ;;
		Australia/Sydney|Australia/Melbourne|Australia/Hobart|Australia/Canberra) echo "AEST-10AEDT,M10.1.0,M4.1.0/3" ;;
		Pacific/Auckland) echo "NZST-12NZDT,M9.5.0,M4.1.0/3" ;;
	esac
}

case "$1" in
up)
	[ -f "$CREDS" ] || exit 2
	SSID=$(sed -n 's/^ssid=//p' "$CREDS" | head -1 | tr -d '\r')
	PSK=$(sed -n 's/^psk=//p' "$CREDS" | head -1 | tr -d '\r')
	[ -n "$SSID" ] || exit 2

	{
		echo "ctrl_interface=/var/run/wpa_supplicant"
		echo "update_config=0"
		echo "network={"
		echo "	ssid=\"$SSID\""
		if [ -n "$PSK" ]; then echo "	psk=\"$PSK\""; else echo "	key_mgmt=NONE"; fi
		echo "}"
	} > "$CONF"
	chmod 600 "$CONF"

	# our own lease script so we don't depend on the firmware's
	cat > "$DHCP_SCRIPT" <<'SCRIPT'
#!/bin/sh
case "$1" in
	bound|renew)
		ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}"
		while route del default 2>/dev/null; do :; done
		for r in $router; do route add default gw "$r" dev "$interface"; done
		: > /tmp/resolv.conf
		for d in $dns; do echo "nameserver $d" >> /tmp/resolv.conf; done
		cp /tmp/resolv.conf /etc/resolv.conf 2>/dev/null
		;;
	deconfig)
		ifconfig "$interface" 0.0.0.0
		;;
esac
SCRIPT
	chmod +x "$DHCP_SCRIPT"

	killall -9 wpa_supplicant udhcpc 2>/dev/null
	ifconfig wlan0 up
	wpa_supplicant -B -i wlan0 -c "$CONF" >/dev/null 2>&1
	udhcpc -i wlan0 -b -s "$DHCP_SCRIPT" >/dev/null 2>&1

	# wait up to 20 seconds for an address
	n=0
	while [ $n -lt 20 ] && [ -z "$(ip_address)" ]; do sleep 1; n=$((n+1)); done
	[ -n "$(ip_address)" ] || exit 1

	# The launcher also watches the real IP state, covering a saved network or phone hotspot that appears later.
	(wifi.sh online >/dev/null 2>&1 &)
	exit 0
	;;
online)
	# Run both connection jobs from the same actual-online event. The launcher's watcher may notice the address at
	# the same moment as `up`; this lock turns those into one run.
	mkdir "$ONLINE_LOCK" 2>/dev/null || exit 0
	trap 'rmdir "$ONLINE_LOCK" 2>/dev/null' EXIT INT TERM
	n=0
	while [ $n -lt 30 ] && [ -z "$(ip_address)" ]; do sleep 1; n=$((n+1)); done
	[ -n "$(ip_address)" ] || exit 1
	# Let the clock settle first: TLS certificate validation depends on a plausible date. Clock-off and an already
	# synced clock both return quickly; the update still runs if time sync fails.
	wifi.sh time >/dev/null 2>&1
	update.sh check >/dev/null 2>&1
	;;
time)
	# the clock, from the internet, whenever wifi connects (unless the clock is turned off). The time zone is the one
	# picked in the settings, or looked up from the connection the first time (then kept, and changeable there)
	MINUI="${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}/.minui"
	LOG="${LOGS_PATH:-/mnt/SDCARD/.userdata/my282/logs}/clock.txt"
	[ "$(cat "$MINUI/clock.txt" 2>/dev/null)" = "0" ] && exit 0
	if [ ! -f "$MINUI/time-zone.txt" ]; then
		J=$(wget -q -T 6 -O - http://worldtimeapi.org/api/ip 2>/dev/null)
		ZONE=$(echo "$J" | sed -n 's/.*"timezone":"\([^"]*\)".*/\1/p')
		OFFSET=$(echo "$J" | sed -n 's/.*"raw_offset":\(-\{0,1\}[0-9]*\).*/\1/p') # seconds east of UTC
		if [ -z "$ZONE" ]; then
			J=$(wget -q -T 6 -O - "http://ip-api.com/line/?fields=timezone,offset" 2>/dev/null)
			ZONE=$(echo "$J" | sed -n 1p)
			OFFSET=$(echo "$J" | sed -n 2p)
		fi
		RULE=$(zone_rule "$ZONE")
		if [ -z "$RULE" ] && [ -n "$OFFSET" ]; then # a zone that isn't listed: its offset, without daylight saving
			S=$(( -OFFSET )); SIGN=""; [ $S -lt 0 ] && { SIGN="-"; S=$(( -S )); }
			H=$(( S / 3600 )); M=$(( S % 3600 / 60 ))
			RULE=$(printf '<UTC%s%02d%02d>%s%d' "$([ $OFFSET -ge 0 ] && echo + || echo -)" $(( ${OFFSET#-} / 3600 )) $(( ${OFFSET#-} % 3600 / 60 )) "$SIGN" $H)
			[ $M -gt 0 ] && RULE="$RULE:$(printf '%02d' $M)"
		fi
		[ -n "$RULE" ] || { echo "time zone: couldn't tell ($ZONE)" >> "$LOG"; exit 1; }
		echo "$RULE" > "$MINUI/time-zone.txt"
		echo "time zone: $ZONE, $RULE" >> "$LOG"
	fi
	export TZ="$(cat "$MINUI/time-zone.txt")"
	n=0
	until ntpd -n -q -p pool.ntp.org >/dev/null 2>&1; do # (busybox ntpd sets the clock once and quits)
		n=$((n+1)); [ $n -ge 4 ] && { echo "sync failed" >> "$LOG"; exit 1; }
		sleep 3 # eg. DNS not answering yet, right after connecting
	done
	hwclock --utc -w
	touch /tmp/minui_clock_set # the clock shows from now until the A30 is off
	echo "synced: $(date +'%F %T %Z')" >> "$LOG"
	;;
down)
	killall -9 wpa_supplicant udhcpc ntpd 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	;;
status)
	ip_address
	;;
esac
