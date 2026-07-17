#!/bin/sh
# Join a local WiFi network as a client (STA mode) at boot, for easier
# SSH/testing access during development.
#
# REPLACES the carplay_wifi AP (etc/wifi_ap.sh) at boot, per a deliberate
# choice made 2026-07-14 -- a single WiFi radio generally can't be an AP
# and a client at the same time, and hostapd/wpa_supplicant both wanting
# wlan0 would conflict. etc/wifi_ap.sh is left in place, untouched, and
# can still be run manually (`/etc/wifi_ap.sh &`) to bring the AP back if
# needed -- this script does not delete or disable that capability, it
# just isn't called automatically from rc.d/rcS anymore.
#
# EDIT etc/wifi_client.conf with your real SSID/password before relying
# on this -- it ships with placeholder values that will never connect.

CONF=/etc/wifi_client.conf
LOG=/tmp/wifi_client.log

echo "=== wifi_client.sh: $(date) ===" > "$LOG"

if [ ! -r "$CONF" ]; then
	echo "ERROR: $CONF not found -- cannot get SSID/password" | tee -a "$LOG"
	exit 1
fi
. "$CONF"

if [ -z "$WIFI_CLIENT_SSID" ] || [ "$WIFI_CLIENT_SSID" = "YOUR_NETWORK_NAME_HERE" ]; then
	echo "ERROR: $CONF still has placeholder SSID -- edit it with your real" | tee -a "$LOG"
	echo "       network name/password, then reboot or re-run this script." | tee -a "$LOG"
	exit 1
fi

# Load WiFi kernel module -- mirrors etc/wifi_ap.sh's own detection logic,
# in case rc.d/rcS's earlier 'modprobe rtl8811cu' didn't run/succeed for
# some reason. Harmless (insmod on an already-loaded module just errors
# and is ignored) if it's already up.
if ! lsmod | grep -q rtl8811cu; then
	if [ -f /tmp/wlan.ko ]; then
		insmod /tmp/wlan.ko 2>>"$LOG"
	else
		for ko in \
			/lib/modules/3.4.0/wlan_rtl8821cs.ko \
			/lib/modules/3.4.0/wlan_rtl8822cs.ko \
			/lib/modules/3.4.0/wlan_rtl8189fs.ko \
			/lib/modules/3.4.0/wlan_rtl8821cu.ko \
			/lib/modules/3.4.0/wlan_rtl8811cu.ko; do
			[ -f "$ko" ] && insmod "$ko" 2>>"$LOG" && break
		done
	fi
fi

# The onboard WiFi module can take a long time to finish USB enumeration
# on this hardware -- the musb controller's own OTG recovery/retry cycle
# has been observed taking 15-20+ seconds in real boot logs, well past a
# fixed `sleep 1`. Poll for wlan0 to actually exist instead of assuming
# it's ready (see etc/wifi_ap.sh for the same fix).
i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do
	sleep 1
	i=$((i + 1))
done

if ! ifconfig wlan0 up 2>>"$LOG"; then
	echo "ERROR: could not bring wlan0 up -- check dmesg for the wifi module" | tee -a "$LOG"
	exit 1
fi

# wpa_passphrase generates a properly hashed/quoted config from plain
# SSID+PSK -- safer than hand-writing the wpa_supplicant.conf syntax, and
# avoids reusing etc/wpa_supplicant.conf (a separate, likely-unrelated
# vendor template -- see the comment in etc/wifi_client.conf).
GENCONF=/tmp/wifi_client_wpa.conf
wpa_passphrase "$WIFI_CLIENT_SSID" "$WIFI_CLIENT_PSK" > "$GENCONF" 2>>"$LOG"

killall wpa_supplicant 2>/dev/null
wpa_supplicant -B -i wlan0 -c "$GENCONF" >>"$LOG" 2>&1
if [ $? -ne 0 ]; then
	echo "ERROR: wpa_supplicant failed to start -- see $LOG" | tee -a "$LOG"
	exit 1
fi

# Give association a few seconds before requesting an address.
sleep 5

killall udhcpc 2>/dev/null
udhcpc -i wlan0 -s /etc/udhcpc.script >>"$LOG" 2>&1 &

sleep 3
# This busybox's ifconfig uses the modern net-tools-style "inet X netmask Y"
# format, not the old "inet addr:X" style -- confirmed via strings on the
# busybox binary itself (2026-07-14) before trusting either format blindly.
IP=$(ifconfig wlan0 2>/dev/null | grep "inet " | head -1)
if [ -n "$IP" ]; then
	echo "Connected: $IP" | tee -a "$LOG"
else
	echo "WARNING: no IP address on wlan0 yet -- check 'wpa_cli status' and" | tee -a "$LOG"
	echo "         '$LOG' for association state; DHCP may still be in progress." | tee -a "$LOG"
fi
