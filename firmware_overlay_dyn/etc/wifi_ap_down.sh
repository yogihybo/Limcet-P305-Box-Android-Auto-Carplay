#!/bin/sh
# Tear down the WiFi AP brought up by wifi_ap.sh -- hostapd + udhcpd
# stopped, wlan0 taken down. Deliberately does NOT unload the WiFi
# kernel module: wifi_ap.sh's own comment notes the onboard chip's USB
# enumeration (via the musb OTG controller) can take 15-20+ seconds on
# this hardware, so leaving the module loaded keeps a later wifi_ap.sh
# re-run fast (modprobe on an already-loaded module is a no-op, and
# wlan0 already exists so its own readiness-poll loop returns
# immediately).
#
# Called by the AA sidecar (aap_wifi_teardown_ap() in aap_wifi_setup.c,
# via ap_state_poll_teardown() in main.c) ~10s after an established AA
# session ends with no new one taking its place. Killing hostapd
# disassociates any connected station, so the phone's own WiFi client
# sees a real disconnect instead of silently staying associated to a
# head unit with no active session behind it -- that's what actually
# makes pressing Connect again on the head unit trigger a genuine fresh
# reconnect (new BT WPP handshake, new AP, new AAP session) instead of
# a no-op against a phone that still thinks it's on the network.

killall hostapd 2>/dev/null
killall udhcpd 2>/dev/null
ifconfig wlan0 down 2>/dev/null
