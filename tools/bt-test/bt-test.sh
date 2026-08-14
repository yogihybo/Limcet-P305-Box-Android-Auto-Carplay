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
# 1.8_ARK1680_TS_REVERSE_ENGINEERING.md). This script targets blueware/ttyHS1,
# the one docs/1.4_WIRELESS_AND_INIT.md documents as actually working, and
# does NOT assume rtkbt is in play.
#
# Nothing in rcS/profile launches `blueware` automatically -- confirmed
# 2026-07-17 the real launch site is app code, not init:
# BlueToothAdapter_Blueware::initBlueToothAdapter() in libBlueTooth.so
# runs system("blueware /etc/blueware-bwNNN.properties > /dev/null 2>&1 &")
# -- note the redirect. blueware itself has well-instrumented,
# errno-annotated error messages for every step of the BTEN (GPIO91)
# export/direction/value sequence ("bpio_init open(%s) for export
# failed: %s (%d)", etc.), but MsnCoreApp's launch throws all of that
# away. This script always (re)starts blueware itself WITHOUT that
# redirect (captured to a log file instead) specifically so those
# messages are visible, and greps for them explicitly -- see
# docs/1.4_WIRELESS_AND_INIT.md section 5 ("Who actually launches
# /usr/bin/blueware") for the full trace.
#
# Also confirmed from libBlueTooth.so: the app-level transport isn't a
# direct /dev/ttyHS1 open -- it's /dev/bw_serial (AT-command channel,
# what this script now round-trips an AT+DEVSTAT over) and /dev/bw_iap
# (raw SPP/iAP passthrough, symlinked to /dev/socket/goc_rfcom), both
# created by blueware once it successfully attaches to the module.
# /dev/ttyHS1 itself is only ever opened by blueware, not the app.

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
echo "--- 3. blueware process (relaunched fresh, WITHOUT the > /dev/null redirect"
echo "    MsnCoreApp normally launches it with, so its own GPIO91 error"
echo "    messages -- if any -- are actually visible for once) ---"
if pgrep blueware >/dev/null 2>&1 || ps | grep -v grep | grep -q blueware; then
	echo "blueware already running -- killing it so we get a fresh, captured launch"
	killall blueware 2>/dev/null
	sleep 1
fi
BW_CONF=/etc/blueware-bw121.properties
[ -f "$BW_CONF" ] || BW_CONF=/etc/blueware-bw123.properties
if [ ! -f "$BW_CONF" ]; then
	fail "no blueware-bwNNN.properties config found in /etc -- can't start blueware"
else
	blueware "$BW_CONF" >/tmp/blueware.log 2>&1 &
	BTPID=$!
	sleep 2
	if kill -0 "$BTPID" 2>/dev/null; then
		pass "blueware started (pid $BTPID, config $BW_CONF) and is still running after 2s"
	else
		fail "blueware exited immediately -- check /tmp/blueware.log:"
		cat /tmp/blueware.log 2>/dev/null
	fi
	if grep -q "bpio_init.*failed" /tmp/blueware.log 2>/dev/null; then
		fail "blueware itself reported a GPIO failure (was previously hidden by MsnCoreApp's > /dev/null launch):"
		grep "bpio_init.*failed" /tmp/blueware.log
	elif grep -q "bpio_init" /tmp/blueware.log 2>/dev/null; then
		pass "blueware's own bpio_init GPIO log lines show no failures"
	else
		unk "no bpio_init log lines seen at all -- either GPIO setup logging changed, or it ran too fast to capture"
	fi
fi

echo
echo "--- 4. App-level transport nodes (/dev/bw_serial, /dev/bw_iap) ---"
echo "These, not /dev/ttyHS1 directly, are what libBlueTooth.so actually"
echo "talks to -- created by blueware once it attaches to the module."
if [ -e /dev/bw_serial ]; then
	pass "/dev/bw_serial exists (AT-command channel)"
else
	fail "/dev/bw_serial missing -- blueware hasn't created it (module attach failure?)"
fi
if [ -e /dev/bw_iap ]; then
	pass "/dev/bw_iap exists (SPP/iAP passthrough)"
else
	fail "/dev/bw_iap missing -- blueware hasn't created it (module attach failure?)"
fi
if [ -L /dev/socket/goc_rfcom ]; then
	pass "/dev/socket/goc_rfcom symlink exists (set up by BlueToothAdapter_Blueware::onStartupConfig())"
else
	unk "/dev/socket/goc_rfcom symlink not present -- only created by the app itself, not blueware, so this is expected unless MsnCoreApp has run its Bluetooth init this boot"
fi

echo
echo "--- 5. AT command round-trip on /dev/bw_serial (AT+DEVSTAT) ---"
echo "Sends the exact wire format recovered from libBlueTooth.so's"
echo "writeCommand(): literal template \"AT+%1\\r\\n\". DEVSTAT is a"
echo "read-only status query -- side-effect-free."
if [ -c /dev/bw_serial ]; then
	# No 'timeout' applet in this busybox build -- background + sleep + kill.
	busybox hexdump -C /dev/bw_serial >/tmp/bt_at_response.log 2>/dev/null &
	RPID=$!
	sleep 1
	printf 'AT+DEVSTAT\r\n' > /dev/bw_serial 2>/dev/null
	if [ $? -ne 0 ]; then
		fail "write to /dev/bw_serial failed"
	fi
	sleep 2
	kill "$RPID" 2>/dev/null
	RESPONSE=$(cat /tmp/bt_at_response.log 2>/dev/null)
	if [ -n "$RESPONSE" ]; then
		echo "$RESPONSE" | head -10
		pass "got a response on /dev/bw_serial after AT+DEVSTAT (module is answering)"
	else
		fail "no response on /dev/bw_serial within 2s of AT+DEVSTAT -- module not answering the AT channel"
	fi
else
	unk "skipped -- /dev/bw_serial not present (see step 4)"
fi

echo
echo "--- 6. Passive traffic listen on ttyHS1 (5s window) ---"
echo "Reading /dev/ttyHS1 for 5 seconds -- any bytes at all confirm the link"
echo "is electrically live and the module is transmitting (idle/heartbeat"
echo "frames are expected on most BT modules even with nothing paired)."
busybox hexdump -C /dev/ttyHS1 >/tmp/bt_traffic.log 2>/dev/null &
HPID=$!
sleep 5
kill "$HPID" 2>/dev/null
TRAFFIC=$(cat /tmp/bt_traffic.log 2>/dev/null)
if [ -n "$TRAFFIC" ]; then
	echo "$TRAFFIC" | head -20
	pass "traffic observed on /dev/ttyHS1"
else
	fail "no traffic observed in 5s -- if step 5 already got a response on /dev/bw_serial"
	echo "    this is expected (blueware owns ttyHS1 directly and mediates via bw_serial/bw_iap,"
	echo "    so idle ttyHS1 doesn't necessarily mean anything is wrong)"
fi

echo
echo "(leaving this script's blueware instance running -- kill it manually"
echo " with 'killall blueware' if you don't want it up)"

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "A pairing/connection test with a real phone is the only thing that"
echo "fully confirms this end-to-end -- this script only confirms the link"
echo "is alive and the userspace stack starts, per this project's own"
echo "boot-log-evidence caveat (a clean start is not proof of correctness)."
[ "$FAIL" -eq 0 ]
