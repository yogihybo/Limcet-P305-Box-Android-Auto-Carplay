#!/bin/sh
# usb-test.sh -- automated USB subsystem check for the live root shell.
# POSIX/ash-compatible (busybox sh). Run on-device.
#
# USB itself is already CONFIRMED working end-to-end on this project (see
# PIN_MASTER_LIST.md's driver source table -- user-confirmed, WiFi dongle
# enumerates over it). This script exists as a *regression* check: run it
# after any kernel/DTS rebuild to quickly confirm USB still enumerates the
# same devices, rather than re-deriving everything from scratch each time.

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

echo "=== usb-test: $(date) ==="

echo
echo "--- 1. USB controllers/hubs present ---"
if command -v lsusb >/dev/null 2>&1; then
	lsusb
else
	unk "lsusb not found -- falling back to /sys/bus/usb/devices listing"
	ls /sys/bus/usb/devices/ 2>/dev/null
fi

HUBCOUNT=$(cat /sys/bus/usb/devices/*/bDeviceClass 2>/dev/null | grep -c "^09$")
if [ "$HUBCOUNT" -ge 2 ]; then
	pass "$HUBCOUNT USB hub(s) enumerated (boot log baseline: 2 -- musb-hdrc host0 + host1)"
elif [ "$HUBCOUNT" -ge 1 ]; then
	unk "$HUBCOUNT USB hub enumerated (baseline is 2 -- one MUSB controller may not be up)"
else
	fail "no USB hubs found at all -- MUSB controller(s) not enumerating anything"
fi

echo
echo "--- 2. WiFi dongle (RTL8811CU) attached and driver bound ---"
if lsmod 2>/dev/null | grep -qi rtl8811cu; then
	pass "rtl8811cu module loaded"
else
	fail "rtl8811cu module not loaded (modprobe rtl8811cu in rc.d/rcS may not have run or failed)"
fi
if ip link show wlan0 >/dev/null 2>&1 || ifconfig wlan0 >/dev/null 2>&1; then
	pass "wlan0 interface exists"
else
	fail "wlan0 interface not present -- driver loaded but device may not be enumerating, check dmesg"
fi

echo
echo "--- 3. USB power switch GPIOs (117, 126) ---"
for GPIO in 117 126; do
	if [ -e "/sys/kernel/debug/gpio" ] && grep -q "gpio-$GPIO" /sys/kernel/debug/gpio 2>/dev/null; then
		pass "GPIO $GPIO (usb_pwr) shows as claimed in /sys/kernel/debug/gpio"
	else
		unk "GPIO $GPIO (usb_pwr) not found in /sys/kernel/debug/gpio -- may still be fine if claimed via a different debug path"
	fi
done

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "Baseline this checks against: 2 hubs, rtl8811cu bound, wlan0 present --"
echo "from the confirmed-working boot captured in"
echo "'docs/new kernel bootlog new uboot v11.txt'. A regression here after a"
echo "kernel/DTS change means re-check PIN_MASTER_LIST.md's USB/MUSB row."
[ "$FAIL" -eq 0 ]
