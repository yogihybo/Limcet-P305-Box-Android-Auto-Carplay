#!/bin/sh
# mmc-test.sh -- automated MMC/SD subsystem check for the live root shell.
# POSIX/ash-compatible (busybox sh). Run on-device.
#
# READ-ONLY BY DESIGN. This project had a real mmc0 corruption incident
# (2026-07-13, see PIN_MASTER_LIST.md's "Open items") during unrelated GPIO
# brute-force probing -- root cause was never conclusively pinned down.
# This script never writes to any block device; it only reads. Do not add
# a write/benchmark test here without re-reading that incident writeup and
# getting a deliberate decision to accept the risk, backed-up SD card first.
#
# mmc0 (SD card slot) is already CONFIRMED working (user-confirmed, see
# PIN_MASTER_LIST.md). mmc1's actual purpose is an open item -- its DTS
# comment ("SDIO WiFi Controller") is known to be wrong now that WiFi is
# confirmed to be the USB RTL8811CU instead (see DRIVER_TEST_PLAN.md
# section 8) -- this script's mmc1 section is investigative, not a
# pass/fail check of a known feature.

set -u
PASS=0
FAIL=0
UNKNOWN=0

pass() { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
unk()  { echo "[UNKNOWN] $1"; UNKNOWN=$((UNKNOWN+1)); }

echo "=== mmc-test: $(date) ==="

echo
echo "--- 1. mmc0 (SD card slot) ---"
if [ -d /sys/class/mmc_host/mmc0 ]; then
	CARD=$(ls /sys/class/mmc_host/mmc0/ 2>/dev/null | grep "^mmc0:")
	if [ -n "$CARD" ]; then
		echo "Card present: $CARD"
		[ -r "/sys/class/mmc_host/mmc0/$CARD/type" ] && echo "  type: $(cat /sys/class/mmc_host/mmc0/$CARD/type)"
		[ -r "/sys/class/mmc_host/mmc0/$CARD/name" ] && echo "  name: $(cat /sys/class/mmc_host/mmc0/$CARD/name)"
		pass "mmc0 has a card enumerated"
	else
		fail "mmc0 host exists but no card enumerated under it -- is a card inserted?"
	fi
else
	fail "/sys/class/mmc_host/mmc0 doesn't exist -- mmc0 host controller not up at all"
fi

echo
echo "--- 2. mmc0 read-only integrity spot-check (10MB, no writes) ---"
if [ -b /dev/mmcblk0 ]; then
	START=$(date +%s 2>/dev/null || echo 0)
	if dd if=/dev/mmcblk0 of=/dev/null bs=1M count=10 2>/tmp/mmc_dd.log; then
		END=$(date +%s 2>/dev/null || echo 0)
		pass "read 10MB from /dev/mmcblk0 cleanly in $((END-START))s (no I/O errors)"
	else
		fail "dd read from /dev/mmcblk0 reported an error -- see /tmp/mmc_dd.log:"
		cat /tmp/mmc_dd.log 2>/dev/null
	fi
else
	unk "/dev/mmcblk0 not found -- check actual block device name (lsblk-equivalent: ls /dev/mmcblk*)"
fi

echo
echo "--- 3. mmc1 -- investigative, not pass/fail (open item, see above) ---"
if [ -d /sys/class/mmc_host/mmc1 ]; then
	CARD1=$(ls /sys/class/mmc_host/mmc1/ 2>/dev/null | grep "^mmc1:")
	if [ -n "$CARD1" ]; then
		echo "mmc1 has something enumerated: $CARD1"
		[ -r "/sys/class/mmc_host/mmc1/$CARD1/type" ] && echo "  type: $(cat /sys/class/mmc_host/mmc1/$CARD1/type)"
		echo "This is new information -- record it in PIN_MASTER_LIST.md's mmc1 row"
		echo "(currently listed as 'purpose unclear')."
	else
		echo "mmc1 host exists but nothing enumerated under it."
		echo "Consistent with mmc1 being unused/dead on this unit -- also a valid,"
		echo "useful answer per DRIVER_TEST_PLAN.md section 8's stated goal."
	fi
else
	echo "/sys/class/mmc_host/mmc1 doesn't exist at all."
fi

echo
echo "--- 4. Currently mounted filesystems on mmc devices ---"
grep -E "mmcblk" /proc/mounts 2>/dev/null || echo "(none mounted)"

echo
echo "=== Summary: $PASS pass, $FAIL fail, $UNKNOWN unknown ==="
echo "This script never writes to a block device. If you need a write/"
echo "benchmark test, that is a deliberate separate decision -- back up the"
echo "SD card first and see PIN_MASTER_LIST.md's incident writeup."
[ "$FAIL" -eq 0 ]
