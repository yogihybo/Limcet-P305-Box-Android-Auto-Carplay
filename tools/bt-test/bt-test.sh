#!/bin/sh
# bt-test.sh -- automated Bluetooth link check for the live root shell.
# POSIX/ash-compatible (busybox sh). Run on-device.
#
# This unit's real BT stack is Feasycom's own `blueware` (AT-command
# protocol, see usr/config.ini's HQ/HR/HS/... opcode table and
# etc/blueware.properties) over /dev/ttyHS1 -- NOT the rtkbt/BlueZ-style
# stack whose config (etc/bluetooth/rtkbt.conf) also happens to ship in
# this rootfs. That rtkbt.conf looks like Android-SDK-template leftover
# (references ro.product.model, bluedroid-style btsnoop paths) -- same
# "shared SDK template, not necessarily used on this unit" pattern already
# found for other vestigial config (TOUCHSERIAL/COMMANDSERIAL, see
# ARK1680_TS_REVERSE_ENGINEERING.md). This script targets blueware/ttyHS1,
# the one docs/wireless_and_init_documentation.md documents as actually
# working, and does NOT assume rtkbt is in play.
#
# Nothing in rcS/profile/msnexport launches `blueware` automatically in
# this rootfs (checked 2026-07-14) -- it needs to be started manually,
# which this script does as a best-effort test step since no confirmed
# CLI usage was found in the binary's strings.

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

echo "=== bt-test: $(date) ==="

echo
echo "--- 1. BTEN (Bluetooth enable, GPIO 91) ---"
if [ ! -e /sys/class/gpio/gpio91 ]; then
	echo 91 > /sys/class/gpio/export 2>/dev/null
	sleep 1 # whole seconds only -- fractional sleep support unconfirmed on this busybox build
fi
if [ -e /sys/class/gpio/gpio91/direction ]; then
	echo out > /sys/class/gpio/gpio91/direction 2>/dev/null
	echo 1 > /sys/class/gpio/gpio91/value 2>/dev/null
	VAL=$(cat /sys/class/gpio/gpio91/value 2>/dev/null)
	if [ "$VAL" = "1" ]; then
		pass "GPIO 91 (BTEN) exported and driven high"
	else
		fail "GPIO 91 value readback ($VAL) doesn't match what was set"
	fi
else
	fail "could not export/access GPIO 91 -- check it isn't claimed by something else (/sys/kernel/debug/gpio)"
fi

echo
echo "--- 2. ttyHS1 device node ---"
if [ -c /dev/ttyHS1 ]; then
	pass "/dev/ttyHS1 exists"
else
	fail "/dev/ttyHS1 missing -- hsuart driver/DTS problem, see PIN_MASTER_LIST.md"
fi

echo
echo "--- 3. blueware process ---"
if pgrep blueware >/dev/null 2>&1 || ps | grep -v grep | grep -q blueware; then
	pass "blueware already running"
	STARTED_BY_US=0
else
	echo "blueware not running -- attempting to start it (best effort, no confirmed"
	echo "CLI usage found in the binary -- if this is wrong, start it manually and"
	echo "re-run just the traffic-monitoring step below)"
	blueware >/tmp/blueware.log 2>&1 &
	BTPID=$!
	STARTED_BY_US=1
	sleep 2
	if kill -0 "$BTPID" 2>/dev/null; then
		pass "blueware started and is still running after 2s (pid $BTPID)"
	else
		fail "blueware exited immediately -- check /tmp/blueware.log:"
		cat /tmp/blueware.log 2>/dev/null
	fi
fi

echo
echo "--- 4. Passive traffic listen on ttyHS1 (5s window) ---"
echo "Reading /dev/ttyHS1 for 5 seconds -- any bytes at all confirm the link"
echo "is electrically live and the module is transmitting (idle/heartbeat"
echo "frames are expected on most BT modules even with nothing paired)."
# No 'timeout' applet in this busybox build -- background + sleep + kill instead.
busybox hexdump -C /dev/ttyHS1 >/tmp/bt_traffic.log 2>/dev/null &
HPID=$!
sleep 5
kill "$HPID" 2>/dev/null
TRAFFIC=$(cat /tmp/bt_traffic.log 2>/dev/null)
if [ -n "$TRAFFIC" ]; then
	echo "$TRAFFIC" | head -20
	pass "traffic observed on /dev/ttyHS1"
else
	fail "no traffic observed in 5s -- try again with blueware freshly (re)started,"
	echo "    or check baud rate (blueware-bw121.properties says 1500000) if a raw"
	echo "    listener rather than blueware itself is expected to show anything"
fi

if [ "${STARTED_BY_US:-0}" = "1" ]; then
	echo
	echo "(leaving the blueware instance this script started running --"
	echo " kill it manually with 'killall blueware' if you don't want it up)"
fi

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "A pairing/connection test with a real phone is the only thing that"
echo "fully confirms this end-to-end -- this script only confirms the link"
echo "is alive and the userspace stack starts, per this project's own"
echo "boot-log-evidence caveat (a clean start is not proof of correctness)."
[ "$FAIL" -eq 0 ]
