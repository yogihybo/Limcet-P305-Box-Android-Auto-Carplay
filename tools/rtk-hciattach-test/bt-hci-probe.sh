#!/bin/sh
# One-shot diagnostic: attempt to bring up hci0 against the real
# RTL8761BTV Bluetooth module via the standard kernel HCI UART driver,
# using rtk_hciattach (github.com/radxa/rtkbt, GPLv2) instead of the
# stock /usr/bin/blueware daemon.
#
# NOT a stack switcher, NOT wired into any UI -- this exists purely to
# answer one open question: does this exact module/firmware actually
# negotiate standard Bluetooth HCI over /dev/ttyHS1 when driven by the
# kernel's own H5 3-wire line discipline, or does blueware's AT-command
# protocol turn out to be load-bearing at the wire level after all?
# See docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md for the full
# investigation this answers.
#
# Requires CONFIG_BT=y, CONFIG_BT_HCIUART=y, CONFIG_BT_HCIUART_3WIRE=y
# in the running kernel -- NOT yet enabled in hardware/kernel_dot_config
# as of this script's own commit, deliberately: no kernel rebuild until
# this script confirms hci0 is reachable at all.
#
# Manually stop blueware yourself before running this (e.g. from a
# shell: pidof blueware, then kill <pid>) -- deliberately not done
# automatically here.
set -e

# 2026-08-19: optional A/B test -- pass "reference" as the first
# argument to stage rtkbt's own bundled generic RTL8761B firmware/
# config (reference-firmware/, github.com/radxa/rtkbt, GPLv2 repo,
# unmodified) instead of this project's real firmware
# (device-firmware/, a copy of firmware_source/mtd6_rootfs/etc/
# rtl8761bt_fw+rtl8761bt_config -- bundled here rather than read from
# /etc/ on the target so this whole tool is self-contained and doesn't
# depend on those files still being present/unmodified on whatever
# device it's copied to). Chip identification still goes through the
# same lmp_subver=0x434d patched table entry either way (src/rtb_fwc.c)
# -- swapping which file lands at $FW_DIR/rtl8761b_fw is the only
# difference. Useful for isolating whether a given failure is specific
# to our real firmware blob or a rtk_hciattach/board-level issue that a
# known-community-tested generic firmware would also hit. Byte-for-byte
# different from ours (different size, different header bytes right
# after the "Realtech" signature) -- see this tool's own README for the
# comparison.
FW_SOURCE="${1:-device}"

FW_DIR=/lib/firmware/rtlbt
TTY=/dev/ttyHS1
GPIO=91
SCRIPT_DIR="$(dirname "$0")"
HCIATTACH="$SCRIPT_DIR/rtk_hciattach"

case "$FW_SOURCE" in
    device)
        FW_SRC_FILE="$SCRIPT_DIR/device-firmware/rtl8761bt_fw"
        CFG_SRC_FILE="$SCRIPT_DIR/device-firmware/rtl8761bt_config"
        ;;
    reference)
        FW_SRC_FILE="$SCRIPT_DIR/reference-firmware/rtl8761b_fw"
        CFG_SRC_FILE="$SCRIPT_DIR/reference-firmware/rtl8761b_config"
        ;;
    *)
        echo "usage: $0 [device|reference]  (default: device)"
        exit 1
        ;;
esac

if pidof blueware >/dev/null 2>&1; then
    echo "blueware is still running and owns $TTY -- stop it first, then re-run this script."
    exit 1
fi

echo "=== bt-hci-probe: enabling gpio$GPIO (BTEN_INTERFACE) ==="
# 2026-08-19 REVISED AGAIN: real Realtek RTL8761ATT datasheet (same
# EN_CHIP-controlled power architecture as our RTL8761BTV) obtained and
# checked directly -- section 3.3.3 "EN_CHIP Control": "EN_CHIP # pin
# is active low to trigger reset behavior and the drive low should be
# longer than 100ms (>100ms) to avoid unconditional reset noise from
# the PCB board." Figure 5 confirms BT power comes up in <1ms after
# release. Our previous 100ms low pulses were sitting exactly AT this
# boundary, not safely above it -- bumped to 150ms with margin.
#
# The datasheet's own Table 14 (UART Interface Power-On Timing
# Parameters) also directly refutes an earlier unverified "200ms
# post-reset boot delay" claim floated during this investigation: the
# real max UART-not-ready duration (T_non-rdy) is only 10ms, and T_por
# (POR release to power management tasks) maxes at 8ms -- the chip is
# genuinely ready within ~18ms of the final high transition, not 200ms.
# The final `sleep 1` (1000ms) below was already ~50x more than this
# real requirement, so left as-is -- harmless margin, not a bug.
#
# Two full low-then-high pulses (not just one) still match the real
# captured stock blueware log (docs/logs/bluetooth log stock_260718.txt,
# lines 66-87: bpio_reset/bpio_set/bpio_reset/bpio_set immediately
# before real chip communication starts) -- the datasheet's own Figure
# 5 only shows a single cycle, so blueware's second cycle is either
# redundant belt-and-suspenders or serves a distinct purpose this
# datasheet doesn't cover; keeping both since it costs nothing and
# matches the one real working reference we have.
if [ ! -d /sys/class/gpio/gpio$GPIO ]; then
    echo $GPIO > /sys/class/gpio/export 2>/dev/null || true
fi
echo out > /sys/class/gpio/gpio$GPIO/direction 2>/dev/null || echo "warn: couldn't set gpio$GPIO direction"
echo 0 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 150000 2>/dev/null || sleep 1
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 150000 2>/dev/null || sleep 1
echo 0 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 150000 2>/dev/null || sleep 1
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
sleep 1

echo "=== bt-hci-probe: staging firmware ($FW_SOURCE: $FW_SRC_FILE -> $FW_DIR/rtl8761b_fw) ==="
mkdir -p "$FW_DIR"
cp "$FW_SRC_FILE" "$FW_DIR/rtl8761b_fw"
cp "$CFG_SRC_FILE" "$FW_DIR/rtl8761b_config"

echo "=== bt-hci-probe: running rtk_hciattach (foreground, Ctrl-C to stop) ==="
echo "Watch for 'HCI Device Index' or check 'ls /sys/class/bluetooth' in another shell."
"$HCIATTACH" -n -s 1500000 "$TTY" rtk_h5
