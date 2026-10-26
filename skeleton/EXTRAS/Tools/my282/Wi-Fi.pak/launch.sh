#!/bin/sh
# Wi-Fi settings: scan, join (with an on-screen keyboard), turn off.
# MinUI's launch.sh stops killing Wi-Fi (and reconnects at boot) while $SHARED_USERDATA_PATH/.minui/wifi-on exists.
cd "$(dirname "$0")"
./wifi.elf
