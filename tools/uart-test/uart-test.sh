#!/bin/sh
# uart-test.sh -- automated passive listen + frame check for all active UART ports.
# POSIX/ash-compatible (busybox sh). Run on-device.
#
# PASSIVE ONLY -- this script never writes to the ports, it only reads.
#
# This busybox build has no 'stty' applet, so baud is set via microcom's
# own '-s SPEED' option (the only baud-setting mechanism available).

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)) ;}

# listen_port <device> <baud> <seconds> <logfile>
# Uses microcom -s <baud> to both configure the baud and capture bytes.
listen_port() {
	dev="$1"; baud="$2"; secs="$3"; log="$4"
	: > "$log"
	busybox microcom -s "$baud" "$dev" > "$log" 2>&1 &
	MCPID=$!
	sleep "$secs"
	kill "$MCPID" 2>/dev/null
	wait "$MCPID" 2>/dev/null
	if grep -qE "can't tcsetattr|cannot open|No such (file|device)|Permission denied" "$log" 2>/dev/null; then
		echo "error $(head -1 "$log")"
		return
	fi
	SZ=$(wc -c < "$log" 2>/dev/null)
	set -- $SZ
	SZ=${1:-0}
	if [ "$SZ" -gt 0 ]; then
		echo "data $SZ"
	else
		echo "empty"
	fi
}

echo "=== uart-test: $(date) ==="

echo
echo "--- 0. Free active userspace serial consumers ---"
echo "Stopping MsnCoreApp and any other known consumers to ensure UART ports are free..."
killall MsnCoreApp 2>/dev/null
sleep 1

# Find all active ttyS* and ttyHS* ports (excluding active console ttyS0)
PORTS=""
for p in /dev/ttyS[0-9]* /dev/ttyHS[0-9]*; do
	[ -e "$p" ] || continue
	if [ "$p" = "/dev/ttyS0" ]; then
		echo "Skipping active console port /dev/ttyS0"
		continue
	fi
	PORTS="$PORTS $p"
done

echo "Active serial ports detected: $PORTS"

for PORT in $PORTS; do
	echo
	echo "=== Testing Port: $PORT ==="
	
	WAS_BLUEWARE_RUNNING=0
	if [ "$PORT" = "/dev/ttyHS1" ]; then
		if pgrep blueware >/dev/null 2>&1 || ps | grep -v grep | grep -q blueware; then
			echo "blueware is running -- temporarily stopping it to free up $PORT..."
			killall blueware 2>/dev/null || true
			WAS_BLUEWARE_RUNNING=1
			sleep 1
		fi
	fi
	
	# Determine baud rates to test
	BAUDS="115200 9600 38400"
	if [ "$PORT" = "/dev/ttyHS1" ]; then
		BAUDS="1500000 115200"
	fi
	
	FOUND=0
	for BAUD in $BAUDS; do
		echo "  Listening at $BAUD for 3s..."
		LOGFILE="/tmp/uart_${PORT##*/}_$BAUD.log"
		RESULT=$(listen_port "$PORT" "$BAUD" 3 "$LOGFILE")
		case "$RESULT" in
		error\ *)
			fail "microcom couldn't open/configure $PORT at $BAUD: ${RESULT#error }"
			break
			;;
		data\ *)
			SIZE=${RESULT#data }
			pass "received $SIZE bytes at $BAUD baud on $PORT"
			echo "  Hex dump (first 64 bytes):"
			busybox hexdump -C "$LOGFILE" 2>/dev/null | head -4
			
			# Specific check for MCU link (ttyHS0) BoxP300 header
			if [ "$PORT" = "/dev/ttyHS0" ]; then
				FIRSTLINE=$(busybox hexdump -C "$LOGFILE" 2>/dev/null | head -1)
				if echo "$FIRSTLINE" | grep -qi "^[0-9a-f]* *2e"; then
					pass "frame appears to start with 0x2E (BoxP300 header sig) -- valid MCU protocol frame!"
				else
					unk "data captured on ttyHS0 does not start with 0x2E BoxP300 header signature."
				fi
			fi
			FOUND=1
			break
			;;
		esac
	done
	
	if [ "$FOUND" -eq 0 ]; then
		unk "$PORT is silent across tested baud rates"
	fi
	
	# Restore blueware if we stopped it
	if [ "$PORT" = "/dev/ttyHS1" ] && [ "$WAS_BLUEWARE_RUNNING" = "1" ]; then
		echo "Restoring blueware..."
		blueware >/tmp/blueware.log 2>&1 &
	fi
done

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
[ "$FAIL" -eq 0 ]
