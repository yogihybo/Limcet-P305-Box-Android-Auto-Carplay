#!/bin/sh
# uart-test.sh -- automated passive listen + frame check for this project's
# two open UART questions: the MCU link (/dev/ttyHS0, DRIVER_TEST_PLAN.md
# section 2) and the MSNEry link (/dev/ttyS2, section 3). POSIX/ash-
# compatible (busybox sh). Run on-device.
#
# PASSIVE ONLY -- this script never writes to either port, it only reads.
# The MCU is documented to send periodic/idle status frames on its own
# (MCU_ADAPTERS.md), so a clean listen is the right first step before any
# physical-input toggling.
#
# This busybox build has no 'stty' applet, so baud is set via microcom's
# own '-s SPEED' option (the only baud-setting mechanism available) rather
# than assuming the port's current termios state is already correct.

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

# listen_port <device> <baud> <seconds> <logfile>
# Uses microcom -s <baud> to both configure the baud and capture bytes --
# no 'timeout' applet in this busybox build, so background + sleep + kill.
# Echoes one of: "error <message>" | "empty" | "data <bytecount>"
#
# IMPORTANT: microcom writes its OWN diagnostics (e.g. "can't tcsetattr
# for /dev/ttyS2: Input/output error" when the port can't be opened/
# configured) to the same stream we're capturing -- a naive byte-count
# check would misreport that error text as "received data" (a real false
# positive caught during local dry-run testing, 2026-07-14). Check for
# microcom's own known failure phrasing first before trusting the byte
# count as genuine serial data.
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
	SZ=$(wc -c < "$log" 2>/dev/null | busybox tr -d ' ')
	if [ "${SZ:-0}" -gt 0 ]; then
		echo "data $SZ"
	else
		echo "empty"
	fi
}

echo "=== uart-test: $(date) ==="

echo
echo "--- 0. Free both ports ---"
echo "killall MsnCoreApp -- confirmed via grep -a on libMcuCenter.so to hold"
echo "both MCUPortName and MSNEryPortName open at runtime. On this project's"
echo "own reconstructed rootfs, /etc/profile no longer auto-respawns it"
echo "(fixed 2026-07-14), so this should stay down across this whole run."
killall MsnCoreApp 2>/dev/null
sleep 1

echo
echo "--- 1. MCU link (/dev/ttyHS0) ---"
if [ ! -c /dev/ttyHS0 ]; then
	fail "/dev/ttyHS0 doesn't exist -- hsuart driver/DTS problem, see PIN_MASTER_LIST.md"
else
	pass "/dev/ttyHS0 exists"
	echo "Listening at 115200 for 5s (per MCU_ADAPTERS.md's documented default)..."
	RESULT=$(listen_port /dev/ttyHS0 115200 5 /tmp/uart_hs0_115200.log)
	case "$RESULT" in
	error\ *)
		fail "microcom couldn't open/configure /dev/ttyHS0 at 115200: ${RESULT#error }"
		;;
	data\ *)
		SIZE=${RESULT#data }
		pass "received $SIZE bytes at 115200"
		echo "hex dump (first 128 bytes):"
		busybox hexdump -C /tmp/uart_hs0_115200.log 2>/dev/null | head -8
		FIRSTLINE=$(busybox hexdump -C /tmp/uart_hs0_115200.log 2>/dev/null | head -1)
		if echo "$FIRSTLINE" | grep -qi "^[0-9a-f]* *2e"; then
			pass "frame appears to start with 0x2E (BoxP300 header sig) -- looks like a real protocol frame, not noise"
			echo "    cross-reference the command byte (offset 1) against MCU_ADAPTERS.md's BoxP300 table"
		else
			unk "captured data doesn't obviously start with 0x2E -- could be a frame boundary mid-capture, or noise. Inspect the hex dump above by hand."
		fi
		;;
	*)
		unk "no bytes at 115200 -- trying 38400 for 5s (per MCU_ADAPTERS.md's fallback baud)..."
		RESULT2=$(listen_port /dev/ttyHS0 38400 5 /tmp/uart_hs0_38400.log)
		case "$RESULT2" in
		error\ *)
			fail "microcom couldn't open/configure /dev/ttyHS0 at 38400: ${RESULT2#error }"
			;;
		data\ *)
			pass "received ${RESULT2#data } bytes at 38400"
			busybox hexdump -C /tmp/uart_hs0_38400.log 2>/dev/null | head -8
			;;
		*)
			fail "silent at both 115200 and 38400 -- link may be dead, MCU unpowered, or wrong wiring. Try toggling a physical input (reverse gear, a steering-wheel button) while re-running, per DRIVER_TEST_PLAN.md section 2 step 5."
			;;
		esac
		;;
	esac
fi

echo
echo "--- 2. MSNEry link (/dev/ttyS2) -- unidentified, see DRIVER_TEST_PLAN.md section 3 ---"
if [ ! -c /dev/ttyS2 ]; then
	fail "/dev/ttyS2 doesn't exist"
else
	pass "/dev/ttyS2 exists"
	FOUND=0
	ERRORED=0
	for BAUD in 115200 9600 19200 38400; do
		echo "Listening at $BAUD for 3s..."
		RESULT=$(listen_port /dev/ttyS2 "$BAUD" 3 "/tmp/uart_s2_$BAUD.log")
		case "$RESULT" in
		error\ *)
			fail "microcom couldn't open/configure /dev/ttyS2 at $BAUD: ${RESULT#error }"
			ERRORED=1
			break
			;;
		data\ *)
			pass "received ${RESULT#data } bytes at $BAUD baud"
			busybox hexdump -C "/tmp/uart_s2_$BAUD.log" 2>/dev/null | head -8
			FOUND=1
			break
			;;
		esac
	done
	if [ "$FOUND" -eq 0 ] && [ "$ERRORED" -eq 0 ]; then
		fail "silent at 115200/9600/19200/38400 -- either genuinely unused on this unit"
		echo "    (a valid, useful answer per DRIVER_TEST_PLAN.md section 3's stated goal),"
		echo "    or needs a physical-input trigger the same way the MCU link might."
	fi
fi

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "This script only proves a link is electrically live and receiving"
echo "bytes -- it does not validate full protocol correctness. Record"
echo "findings in docs/boot_experiment_log.md and update PIN_MASTER_LIST.md's"
echo "hsuart/MSNEry rows."
[ "$FAIL" -eq 0 ]
