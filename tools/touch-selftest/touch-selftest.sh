#!/bin/sh
# touch-selftest.sh -- automated wrapper around this project's existing touch
# test tools (tools/ark1680-ts-test/, plus the rootfs's own tslib binaries),
# following the debug flow already documented in
# tools/ark1680-ts-test/README.md, with pass/fail parsing instead of
# requiring the operator to manually run and interpret each command.
# POSIX/ash-compatible (busybox sh). Run on-device. Requires a real physical
# touch during the timed windows below -- this cannot simulate one for you.

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

ARKTS="$(dirname "$0")/../ark1680-ts-test/ark-ts-test"

echo "=== touch-selftest: $(date) ==="

echo
echo "--- 1. dmesg probe check ---"
if dmesg | grep -qi "ark1680.*touchscreen registered"; then
	dmesg | grep -i ark1680_ts | tail -5
	pass "driver probed successfully"
else
	fail "no 'registered' line found in dmesg for ark1680_ts -- driver may not have loaded"
fi

echo
echo "--- 2. Raw ADC hardware check (independent of driver) ---"
if [ -x "$ARKTS" ]; then
	echo "Sampling raw_x/raw_y twice, 3s apart -- TOUCH THE PANEL NOW and hold"
	echo "your finger down through this step:"
	R1=$("$ARKTS" regs 2>/dev/null | grep -i "raw_x\|raw_y")
	sleep 3
	R2=$("$ARKTS" regs 2>/dev/null | grep -i "raw_x\|raw_y")
	echo "before: $R1"
	echo "after:  $R2"
	if [ "$R1" != "$R2" ]; then
		pass "raw_x/raw_y changed -- ADC hardware is alive and responding to touch"
	else
		fail "raw_x/raw_y identical -- either nothing touched the panel during the window,"
		echo "    or the fault is upstream of software entirely (pinmux/clock, or the"
		echo "    panel isn't wired/seated). Re-run and make sure you're actually"
		echo "    touching the panel during the 3s window."
	fi
else
	unk "tools/ark1680-ts-test/ark-ts-test not found at $ARKTS"
fi

echo
echo "--- 3. Enable driver debug tracing ---"
if [ -w /sys/module/ark1680_ts/parameters/debug ]; then
	echo 1 > /sys/module/ark1680_ts/parameters/debug
	pass "debug tracing enabled -- watch 'dmesg -w' separately during real use for"
	echo "    per-IRQ raw sample / filtered coordinate / stable-unstable detail"
else
	unk "/sys/module/ark1680_ts/parameters/debug not writable/present -- module may be built-in, not a loadable module"
fi

echo
echo "--- 4. Real evdev events (needs a physical touch during this 5s window) ---"
EVENTNODE=$(grep -A1 "ark1680-ts" /proc/bus/input/devices 2>/dev/null | grep -o 'event[0-9]*' | head -1)
if [ -n "$EVENTNODE" ]; then
	echo "Found input node: /dev/input/$EVENTNODE"
	if [ -x "$ARKTS" ]; then
		echo "TOUCH THE PANEL NOW -- capturing events for 5s..."
		# No 'timeout' applet in this busybox build -- background + sleep + kill.
		"$ARKTS" events "/dev/input/$EVENTNODE" >/tmp/ts_events.log 2>&1 &
		EPID=$!
		sleep 5
		kill "$EPID" 2>/dev/null
		EVENTS=$(cat /tmp/ts_events.log 2>/dev/null)
		echo "$EVENTS" | head -20
		if echo "$EVENTS" | grep -qE "ABS_X|ABS_Y|BTN_TOUCH|SYN_REPORT"; then
			pass "real evdev touch events received"
		else
			fail "no ABS_X/ABS_Y/BTN_TOUCH/SYN_REPORT events seen in 5s -- driver probes"
			echo "    but isn't delivering real input events. See"
			echo "    ARK1680_TS_REVERSE_ENGINEERING.md for prior findings (mostly"
			echo "    gathered on stock firmware, not necessarily this kernel build)."
		fi
	else
		unk "ark-ts-test not found -- can't read the event node"
	fi
else
	fail "no ark1680-ts entry found in /proc/bus/input/devices -- input device not registered"
fi

echo
echo "--- 5. tslib raw sample dump (no calibration needed, uses TSLIB_TSDEVICE"
echo "    from the environment -- /dev/input/event0 per /etc/profile) ---"
if command -v ts_print_raw >/dev/null 2>&1; then
	echo "TOUCH THE PANEL NOW -- capturing for 5s..."
	# No 'timeout' applet in this busybox build -- background + sleep + kill.
	ts_print_raw >/tmp/ts_print_raw.log 2>&1 &
	TPID=$!
	sleep 5
	kill "$TPID" 2>/dev/null
	TSOUT=$(cat /tmp/ts_print_raw.log 2>/dev/null)
	echo "$TSOUT" | head -20
	if [ -n "$TSOUT" ]; then
		pass "ts_print_raw produced output"
	else
		unk "ts_print_raw produced no output in 5s"
	fi
else
	unk "ts_print_raw not found in PATH"
fi

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "Record findings in docs/boot_experiment_log.md and update"
echo "PIN_MASTER_LIST.md's 'Resistive touch' operational-status row."
[ "$FAIL" -eq 0 ]
