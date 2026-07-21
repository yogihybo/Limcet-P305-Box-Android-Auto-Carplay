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

# GPIO34 -- BD37033 enable/reset line, driven low once (and only once) at
# Sound_BD37033 construction with zero error checking anywhere in the call
# chain (gpio_export()/gpio_set_dir()/gpio_set_value() in libMsnCommons.so
# all discard write()'s return value; Sound_BD37033's ctor never checks
# setDir()/setValue()'s return either) -- see docs/BD37033.md section 2.
# Re-asserted here as a HIGH-then-LOW pulse (not just a flat low write)
# before every sound test below, since if the app already left it sitting
# at 0 from a previous run, re-writing 0 is a no-op with no edge -- this
# guarantees a real transition regardless of whatever state it was already in.
GPIO34_PATH=/sys/class/gpio/gpio34
gpio34_toggle() {
	if [ ! -d "$GPIO34_PATH" ]; then
		echo 34 > /sys/class/gpio/export 2>/dev/null
	fi
	if [ -d "$GPIO34_PATH" ]; then
		echo out > "$GPIO34_PATH/direction" 2>/dev/null
		echo 1 > "$GPIO34_PATH/value" 2>/dev/null
		sleep 1
		echo 0 > "$GPIO34_PATH/value" 2>/dev/null
		echo "  gpio34: pulsed high->low (BD37033 enable/reset, see docs/BD37033.md sec 2)"
	else
		echo "  gpio34: could not export -- skipping pulse (check /sys/class/gpio/export permissions)"
	fi
}

echo "=== audio-test: $(date) ==="

echo
echo "--- 1. Sound card / device enumeration ---"
CARD=""
DEV=0
DEVICES=""
if command -v aplay >/dev/null 2>&1; then
	AP_L=$(aplay -l 2>&1)
	echo "$AP_L"
	# Build the full list of real (non-Dummy) "card,device" pairs directly
	# from aplay -l -- this is the canonical, always-current source (unlike
	# /proc/asound/cards, which only lists cards, not their playback
	# devices, and doesn't reflect dai-link ordering the way aplay -l does).
	# A "Dummy" entry alone means no real card ever registered -- this
	# project hit exactly that state early on (see
	# docs/AUDIO_SUBSYSTEM_INVESTIGATION.md, "aplay -l before any fixes").
	DEVICES=$(echo "$AP_L" | sed -n 's/^card \([0-9]\+\): \([^[]*\).*device \([0-9]\+\):.*/\1 \2 \3/p' | \
		while read -r c name d; do
			case "$name" in
				*Dummy*) ;;
				*) echo "${c},${d}" ;;
			esac
		done)
	if [ -n "$DEVICES" ]; then
		pass "real ALSA playback device(s) found: $(echo "$DEVICES" | tr '\n' ' ')"
		CARD=$(echo "$DEVICES" | sed -n '1p' | cut -d, -f1)
		DEV=$(echo "$DEVICES" | sed -n '1p' | cut -d, -f2)
	else
		fail "no real (non-Dummy) playback device in aplay -l output above"
	fi
else
	unk "aplay not found in PATH -- cannot enumerate devices"
fi

echo
echo "--- 2. BD37033 bus presence (i2c-gpio2, addr 0x40) ---"
# Reuses the tool already built for this exact bus/address question --
# see tools/i2c-scan/README.md and PIN_MASTER_LIST.md's driver source table.
# Resolve i2c-scan across either layout this project ships it in: the
# tools/i2c-scan/i2c-scan tree (manual copy), flat in the same directory as
# this script, or flat in /usr/bin (build_bootable_sdcard.sh's
# install_diag_tools installs everything into one flat directory, not a
# tools/ subtree -- checked 2026-07-14 while wiring this script into it).
I2CSCAN="$(dirname "$0")/../i2c-scan/i2c-scan"
[ -x "$I2CSCAN" ] || I2CSCAN="$(dirname "$0")/i2c-scan"
[ -x "$I2CSCAN" ] || I2CSCAN="$(command -v i2c-scan 2>/dev/null)"
if [ -n "$I2CSCAN" ] && [ -x "$I2CSCAN" ]; then
	if "$I2CSCAN" 2>&1 | grep -q "0x40"; then
		pass "BD37033 ACKs at 0x40 on i2c-gpio2"
	else
		fail "no ACK seen at 0x40 -- BD37033 not responding on the bus (i2c-scan output above)"
	fi
else
	unk "i2c-scan not found (checked ../i2c-scan/, alongside this script, and \$PATH) -- copy it somewhere findable or run it separately"
fi

echo
echo "--- 3. Mixer control presence (PA Volume / PA Mute) ---"
# Real control names confirmed from the driver source itself
# (sound/soc/arkmicro/BD37033.c) -- not guessed.
if command -v amixer >/dev/null 2>&1; then
	CONTROLS=$(amixer scontrols 2>&1)
	echo "$CONTROLS"
	# ALSA's simple-mixer layer collapses a "<Name> Volume" element into a
	# bare "<Name>" simple control (same as "Master Playback Volume" ->
	# "Master"), so SOC_SINGLE_EXT("PA Volume", ...) shows up here as just
	# 'PA' -- confirmed 2026-07-14 via a live amixer scontrols capture
	# after the aux-devs DTS fix. Match either form.
	if echo "$CONTROLS" | grep -qiE "'PA'|PA Volume"; then
		pass "'PA' volume control exists -- codec driver bound and exposing a control"
	else
		fail "'PA'/'PA Volume' control not found -- BD37033 driver likely didn't bind (cross-check dmesg | grep -i bd37033)"
	fi
else
	unk "amixer not found in PATH"
fi

echo
echo "--- 4. Exercise the control path (does NOT prove audible correctness --"
echo "    you must listen) ---"
if command -v amixer >/dev/null 2>&1 && echo "$CONTROLS" | grep -qiE "'PA'|PA Volume"; then
	ORIG=$(amixer sget 'PA' 2>/dev/null | grep -o '[0-9]\+' | sed -n '1p')
	echo "Current PA (volume): ${ORIG:-unknown}"
	echo "Setting PA to 10, then back to ${ORIG:-40}..."
	amixer sset 'PA' 10 >/dev/null 2>&1
	READBACK=$(amixer sget 'PA' 2>/dev/null | grep -o '[0-9]\+' | sed -n '1p')
	if [ "$READBACK" = "10" ]; then
		pass "amixer read-back after set matches (10) -- ALSA's own state is consistent"
		echo "    NOTE: this only proves ALSA's cached value round-trips correctly."
		echo "    It does NOT prove the I2C write actually reached the BD37033 chip --"
		echo "    per DRIVER_TEST_PLAN.md section 6, only an AUDIBLE volume change while"
		echo "    audio plays proves the control path. Play audio now and listen."
	else
		fail "amixer read-back (${READBACK:-none}) doesn't match what was set (10)"
	fi
	[ -n "$ORIG" ] && amixer sset 'PA' "$ORIG" >/dev/null 2>&1
else
	unk "skipped -- 'PA' control not available"
fi

echo
echo "--- 5. Static noise playback, cycled across every device found (requires your ears) ---"
if [ -n "$DEVICES" ] && command -v aplay >/dev/null 2>&1; then
	# Split on newlines with a `for` loop (not `... | while read`) so this
	# runs in the current shell, not a subshell -- pass()/fail() need to
	# update this shell's PASS/FAIL counters, which a piped subshell can't do.
	OLD_IFS=$IFS
	IFS='
'
	for pair in $DEVICES; do
		IFS=$OLD_IFS
		c=${pair%,*}
		d=${pair#*,}
		NOISE_DEV="hw:${c},${d}"
		echo
		gpio34_toggle
		echo "Playing 3s of noise on ${NOISE_DEV} -- listen for actual sound output..."
		echo "  aplay -D ${NOISE_DEV} -f S16_LE -r 44100 -c 2 -d 3 /dev/urandom"
		OUT=$(aplay -D "$NOISE_DEV" -f S16_LE -r 44100 -c 2 -d 3 /dev/urandom 2>&1)
		RC=$?
		echo "$OUT"
		# aplay's own -d 3 duration flag is not trusted to actually block for
		# the full 3 real-time seconds on this device (observed returning
		# near-instantly) -- sleep explicitly so there's always a real
		# listening window per device, regardless of aplay's behavior.
		sleep 3
		if [ "$RC" -eq 0 ]; then
			pass "aplay exited cleanly on ${NOISE_DEV} (does not by itself prove audible output -- confirm you heard it)"
		else
			fail "aplay on ${NOISE_DEV} exited with error (rc=$RC) -- see output above"
		fi
		IFS='
'
	done
	IFS=$OLD_IFS
else
	unk "skipped -- no real device/aplay available"
fi

echo
echo "--- 6. Playback test (requires a WAV file and your ears) ---"
WAV="${1:-}"
if [ -n "$WAV" ] && [ -r "$WAV" ]; then
	if command -v aplay >/dev/null 2>&1; then
		WAV_DEV=""
		[ -n "$CARD" ] && WAV_DEV="hw:${CARD},${DEV}"
		gpio34_toggle
		echo "Playing $WAV via aplay${WAV_DEV:+ -D $WAV_DEV} -- listen for actual sound output..."
		if [ -n "$WAV_DEV" ]; then
			aplay -D "$WAV_DEV" "$WAV" && pass "aplay exited cleanly on ${WAV_DEV} (does not by itself prove audible output -- confirm you heard it)" \
				|| fail "aplay on ${WAV_DEV} reported an error"
		else
			aplay "$WAV" && pass "aplay exited cleanly (does not by itself prove audible output -- confirm you heard it)" \
				|| fail "aplay reported an error"
		fi
		# Give a consistent listening window here too, in case the WAV is
		# short or aplay returns before playback is actually audible.
		sleep 3
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
