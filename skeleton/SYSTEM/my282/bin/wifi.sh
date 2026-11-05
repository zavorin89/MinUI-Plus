#!/bin/sh
# wifi.sh up [first]|down|status|ssid|online|time
# online: runs the quiet tasks that need a newly working connection (clock sync and update check)
# Connects wlan0 to whichever saved network in /mnt/SDCARD/.userdata/shared/wifi.txt is in range, and moves between
# them by itself (the first listed, the latest joined, is preferred when several are):
#   ssid=Home
#   psk=my password        (leave out for open networks)
#   ssid=Phone
#   psk=another password
# up first: only succeeds once joined to the first one (the network just picked in the Wi-Fi tool).
# status prints the IPv4 address, or nothing when not connected. ssid prints the joined network's name.

CREDS=/mnt/SDCARD/.userdata/shared/wifi.txt
CONF=/tmp/wpa_supplicant.conf
SSIDS=/tmp/wifi_ssids # the saved names in the order wpa_supplicant numbers them, for ssid
WPA_CLI="wpa_cli -p /var/run/wpa_supplicant -i wlan0"
DHCP_SCRIPT=/tmp/udhcpc.sh
RUNTIME_PATH="${MINUI_RUNTIME_PATH:-/tmp}"
ONLINE_LOCK="$RUNTIME_PATH/minui_wifi_online.lock"
TIME_LOCK="$RUNTIME_PATH/minui_wifi_time.lock"
CLOCK_SET=/tmp/minui_clock_set

# the address, only while joined to a network: an address stays on wlan0 after it drops (or is taken down), and that
# must not count as connected (minui goes by the same operstate)
ip_address() {
	[ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" = up ] || return
	ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1
}

# wpa_supplicant's number for the network it's joined (its place in the conf, 0 for the first), empty when none
network_id() {
	$WPA_CLI status 2>/dev/null | sed -n 's/^id=//p' | head -1
}

# the conf: every saved network, each preferred over the ones after it (wpa_supplicant joins the highest priority in
# range, and another when that one goes). Fails when none is saved
write_conf() {
	COUNT=$(grep -c '^ssid=.' "$CREDS" 2>/dev/null)
	[ "${COUNT:-0}" -gt 0 ] || return 1
	: > "$SSIDS"
	{
		echo "ctrl_interface=/var/run/wpa_supplicant"
		echo "update_config=0"
		tr -d '\r' < "$CREDS" | {
			P=$COUNT
			SSID=
			PSK=
			network() {
				[ -n "$SSID" ] || return
				echo "network={"
				echo "	ssid=\"$SSID\""
				if [ -n "$PSK" ]; then echo "	psk=\"$PSK\""; else echo "	key_mgmt=NONE"; fi
				echo "	priority=$P"
				echo "}"
				echo "$SSID" >> "$SSIDS"
				P=$((P-1))
			}
			while IFS= read -r LINE || [ -n "$LINE" ]; do
				case "$LINE" in
					ssid=*) network; SSID="${LINE#ssid=}"; PSK= ;;
					psk=*) PSK="${LINE#psk=}" ;;
				esac
			done
			network
		}
	} > "$CONF"
	chmod 600 "$CONF"
}

# a lock (a folder holding its owner's pid): 0 when taken. One left by a run that was killed is taken over, so a
# power cut or a hang never blocks every later run until a reboot
take_lock() {
	if mkdir "$1" 2>/dev/null; then echo $$ > "$1/pid"; return 0; fi
	OWNER=$(cat "$1/pid" 2>/dev/null)
	[ -z "$OWNER" ] && { sleep 1; OWNER=$(cat "$1/pid" 2>/dev/null); } # (just made, its pid on the way)
	[ -n "$OWNER" ] && kill -0 "$OWNER" 2>/dev/null && return 1
	rm -rf "$1"
	mkdir "$1" 2>/dev/null && echo $$ > "$1/pid"
}

# one clock sync from one server, at most 10 seconds: busybox ntpd -q waits forever for one that doesn't answer
ntp_once() {
	ntpd -n -q -p "$1" >/dev/null 2>&1 &
	PID=$!
	t=0
	while kill -0 $PID 2>/dev/null; do
		[ $t -ge 10 ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; return 1; }
		sleep 1; t=$((t+1))
	done
	wait $PID
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
	write_conf || exit 2

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
	ip addr flush dev wlan0 2>/dev/null # the last network's address isn't this one's
	ifconfig wlan0 up
	wpa_supplicant -B -i wlan0 -c "$CONF" >/dev/null 2>&1
	udhcpc -i wlan0 -b -s "$DHCP_SCRIPT" >/dev/null 2>&1

	# wait up to 20 seconds for an address (with first, on the first network: a wrong password there can still join
	# another saved one, which isn't what was asked for. Without wpa_cli to tell, any will do)
	CHECK_FIRST=
	[ "$2" = first ] && command -v wpa_cli >/dev/null 2>&1 && CHECK_FIRST=1
	n=0
	until [ -n "$(ip_address)" ] && { [ -z "$CHECK_FIRST" ] || [ "$(network_id)" = 0 ]; }; do
		n=$((n+1)); [ $n -ge 20 ] && exit 1
		sleep 1
	done

	# The launcher also watches the real IP state, covering a saved network or phone hotspot that appears later.
	(wifi.sh online >/dev/null 2>&1 &)
	exit 0
	;;
online)
	# Run both connection jobs from the same actual-online event. The launcher's watcher may notice the address at
	# the same moment as `up`; this lock turns those into one run.
	take_lock "$ONLINE_LOCK" || exit 0
	trap 'rm -rf "$ONLINE_LOCK"' EXIT INT TERM
	n=0
	while [ $n -lt 30 ] && [ -z "$(ip_address)" ]; do sleep 1; n=$((n+1)); done
	[ -n "$(ip_address)" ] || exit 1
	# Let the clock settle first: TLS certificate validation depends on a plausible date. Clock-off and an already
	# synced clock both return quickly; the update still runs if time sync fails.
	wifi.sh time >/dev/null 2>&1
	update.sh check >/dev/null 2>&1
	;;
time)
	# Sync game RTCs even when the displayed clock is off. The time zone is the one
	# picked in the settings, or looked up from the connection the first time (then kept, and changeable there)
	MINUI="${SHARED_USERDATA_PATH:-/mnt/SDCARD/.userdata/shared}/.minui"
	LOG="${LOGS_PATH:-/mnt/SDCARD/.userdata/my282/logs}/clock.txt"
	# one at a time (a reconnect and a new time zone can ask together): a second one waits its turn, since it may be
	# for a zone the first didn't know about
	n=0
	until take_lock "$TIME_LOCK"; do n=$((n+1)); [ $n -ge 60 ] && exit 1; sleep 1; done
	trap 'rm -rf "$TIME_LOCK"' EXIT INT TERM
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
		# not known (both lookups down): the clock still syncs, for games' clocks, but stays hidden until a later
		# try finds the zone (launch.sh tries again while it's hidden), or one is picked in the settings
		if [ -n "$RULE" ]; then
			echo "$RULE" > "$MINUI/time-zone.txt"
			echo "time zone: $ZONE, $RULE" >> "$LOG"
		else echo "time zone: couldn't tell ($ZONE)" >> "$LOG"
		fi
	fi
	[ -f "$MINUI/time-zone.txt" ] && export TZ="$(cat "$MINUI/time-zone.txt")"
	BEFORE=$(date +%s)
	UP_BEFORE=$(cut -d. -f1 /proc/uptime)
	n=0
	for SERVER in pool.ntp.org time.google.com time.cloudflare.com pool.ntp.org; do # (busybox ntpd sets it once and quits)
		ntp_once "$SERVER" && break
		n=$((n+1)); [ $n -ge 4 ] && { echo "sync failed" >> "$LOG"; exit 1; }
		sleep 3 # eg. DNS not answering yet, right after connecting
	done
	hwclock --utc -w
	TZ=UTC0 date +'%F %T' > "${DATETIME_PATH:-/mnt/SDCARD/.userdata/shared/datetime-utc.txt}" # the best guess after a power cut
	touch "$MINUI/clock-synced" # it's the real time, from here it's trusted across power offs (launch.sh)
	rm -f "$MINUI/clock-manual" # (and it's in UTC now, not as it was typed)
	sync
	[ -f "$MINUI/time-zone.txt" ] && touch "$CLOCK_SET" # the clock shows from now until the A30 is off
	echo "synced: $(date +'%F %T %Z'), it was off by $(( $(date +%s) - BEFORE - ( $(cut -d. -f1 /proc/uptime) - UP_BEFORE ) )) s" >> "$LOG"
	;;
down)
	killall -9 wpa_supplicant udhcpc ntpd 2>/dev/null
	ip addr flush dev wlan0 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	;;
status)
	ip_address
	;;
ssid)
	[ -n "$(ip_address)" ] || exit 0
	ID=$(network_id)
	case "$ID" in ''|*[!0-9]*) exit 0 ;; esac
	sed -n "$((ID + 1))p" "$SSIDS" 2>/dev/null
	;;
esac
