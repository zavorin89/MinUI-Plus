#!/bin/sh
# wifi.sh up|down|status
# Connects wlan0 using the network in /mnt/SDCARD/.userdata/shared/wifi.txt:
#   ssid=My Network
#   psk=my password        (leave out for open networks)
# status prints the IPv4 address, or nothing when not connected.

CREDS=/mnt/SDCARD/.userdata/shared/wifi.txt
CONF=/tmp/wpa_supplicant.conf
DHCP_SCRIPT=/tmp/udhcpc.sh

ip_address() {
	ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1
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

	# set the clock so unlock times and sessions are right (busybox ntpd quits once set)
	(ntpd -n -q -p pool.ntp.org >/dev/null 2>&1 &)
	exit 0
	;;
down)
	killall -9 wpa_supplicant udhcpc ntpd 2>/dev/null
	ifconfig wlan0 down 2>/dev/null
	;;
status)
	ip_address
	;;
esac
