#!/bin/sh
# audio-test.sh -- automated audio subsystem check for the live root shell.
# POSIX/ash-compatible (busybox sh), no bashisms. Run on-device.
#
# Distinguishes what this project has learned to distinguish carefully:
#   - the I2S DATA PATH (already independently confirmed working, see
#     docs/AUDIO_SUBSYSTEM_INVESTIGATION.md and PIN_MASTER_LIST.md)
#   - the BD37033 CODEC CONTROL PATH over i2c-gpio2 (NOT confirmed as of
#     2026-07-14 -- see docs/DRIVER_TEST_PLAN.md section 6)
# A clean exit code here is NOT proof of correct operation by itself --
# per this project's own standing correction (feedback_bootlog_evidence_weak
# memory / PIN_MASTER_LIST.md's boot-log caveat), only an actual audible
# result confirms anything. This script automates the mechanical steps and
# reports what it can verify in software; it cannot listen for you.

set -u
PASS=0
FAIL=0
UNKNOWN=0
CONTROLS=""

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

echo "=== audio-test: $(date) ==="

echo
echo "--- 1. Sound card registration ---"
if [ -r /proc/asound/cards ] && [ -s /proc/asound/cards ]; then
	cat /proc/asound/cards
	pass "at least one ALSA sound card registered"
else
	fail "/proc/asound/cards missing or empty -- no sound card registered at all"
fi

echo
echo "--- 2. BD37033 bus presence (i2c-gpio2, addr 0x41) ---"
# Reuses the tool already built for this exact bus/address question --
# see tools/i2c-scan/README.md and PIN_MASTER_LIST.md's driver source table.
I2CSCAN="$(dirname "$0")/../i2c-scan/i2c-scan"
if [ -x "$I2CSCAN" ]; then
	if "$I2CSCAN" 2>&1 | grep -q "0x41"; then
		pass "BD37033 ACKs at 0x41 on i2c-gpio2"
	else
		fail "no ACK seen at 0x41 -- BD37033 not responding on the bus (i2c-scan output above)"
	fi
else
	unk "tools/i2c-scan not found at $I2CSCAN -- copy it alongside this script or run it separately"
fi

echo
echo "--- 3. Mixer control presence (PA Volume / PA Mute) ---"
# Real control names confirmed from the driver source itself
# (sound/soc/arkmicro/BD37033.c) -- not guessed.
if command -v amixer >/dev/null 2>&1; then
	CONTROLS=$(amixer scontrols 2>&1)
	echo "$CONTROLS"
	if echo "$CONTROLS" | grep -qi "PA Volume"; then
		pass "'PA Volume' control exists -- codec driver bound and exposing a control"
	else
		fail "'PA Volume' control not found -- BD37033 driver likely didn't bind (cross-check dmesg | grep -i bd37033)"
	fi
else
	unk "amixer not found in PATH"
fi

echo
echo "--- 4. Exercise the control path (does NOT prove audible correctness --"
echo "    you must listen) ---"
if command -v amixer >/dev/null 2>&1 && echo "$CONTROLS" | grep -qi "PA Volume"; then
	ORIG=$(amixer sget 'PA Volume' 2>/dev/null | grep -o '[0-9]\+' | head -1)
	echo "Current PA Volume: ${ORIG:-unknown}"
	echo "Setting PA Volume to 10, then back to ${ORIG:-40}..."
	amixer sset 'PA Volume' 10 >/dev/null 2>&1
	READBACK=$(amixer sget 'PA Volume' 2>/dev/null | grep -o '[0-9]\+' | head -1)
	if [ "$READBACK" = "10" ]; then
		pass "amixer read-back after set matches (10) -- ALSA's own state is consistent"
		echo "    NOTE: this only proves ALSA's cached value round-trips correctly."
		echo "    It does NOT prove the I2C write actually reached the BD37033 chip --"
		echo "    per DRIVER_TEST_PLAN.md section 6, only an AUDIBLE volume change while"
		echo "    audio plays proves the control path. Play audio now and listen."
	else
		fail "amixer read-back (${READBACK:-none}) doesn't match what was set (10)"
	fi
	[ -n "$ORIG" ] && amixer sset 'PA Volume' "$ORIG" >/dev/null 2>&1
else
	unk "skipped -- 'PA Volume' control not available"
fi

echo
echo "--- 5. Playback test (requires a WAV file and your ears) ---"
WAV="${1:-}"
if [ -n "$WAV" ] && [ -r "$WAV" ]; then
	if command -v aplay >/dev/null 2>&1; then
		echo "Playing $WAV via aplay -- listen for actual sound output..."
		aplay "$WAV" && pass "aplay exited cleanly (does not by itself prove audible output -- confirm you heard it)" \
			|| fail "aplay reported an error"
	else
		unk "aplay not found in PATH"
	fi
else
	unk "no WAV file given (usage: $0 /path/to/test.wav) -- skipped playback test"
fi

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "Remember: PASS here means a mechanical check succeeded, not that audio"
echo "was confirmed audibly correct. Cross-check against DRIVER_TEST_PLAN.md"
echo "section 6 and record findings in docs/boot_experiment_log.md."
[ "$FAIL" -eq 0 ]
