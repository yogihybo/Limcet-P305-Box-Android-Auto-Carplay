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

FW_DIR=/lib/firmware/rtlbt
TTY=/dev/ttyHS1
GPIO=91
HCIATTACH="$(dirname "$0")/rtk_hciattach"

if pidof blueware >/dev/null 2>&1; then
    echo "blueware is still running and owns $TTY -- stop it first, then re-run this script."
    exit 1
fi

echo "=== bt-hci-probe: enabling gpio$GPIO (BTEN_INTERFACE) ==="
# 2026-08-19 REVISED: Ghidra-decompiled the real bpio_init()/
# bpio_reset()/bpio_set() functions from the actual /usr/bin/blueware
# binary (firmware_source/mtd6_rootfs/usr/bin/blueware, all three
# confirmed operating on the same gpio91 value fd). bpio_init() itself
# ends by writing "0" (LOW) -- the daemon does NOT come up with the
# pin high. The real captured stock log (docs/logs/bluetooth log
# stock_260718.txt, lines 66-87) shows the meaningful transition
# immediately before real chip communication starts is TWO full
# low-then-high pulses back to back (bpio_reset/bpio_set/bpio_reset/
# bpio_set), ending high, right before "realtek selected"/opening
# librtkvnd.so. This supersedes the previous revision of this script
# (which removed the pulse entirely based on a doc excerpt that turned
# out to only cover bpio_init's static enable step, not the real
# pre-communication reset dance) -- neither bpio_reset() nor bpio_set()
# has any delay in its own body, so the exact inter-call timing is
# still unconfirmed (blocked on resolving what looks like vtable-
# dispatched calls to these two functions); 100ms between each
# transition here is a conservative placeholder, not a confirmed value.
if [ ! -d /sys/class/gpio/gpio$GPIO ]; then
    echo $GPIO > /sys/class/gpio/export 2>/dev/null || true
fi
echo out > /sys/class/gpio/gpio$GPIO/direction 2>/dev/null || echo "warn: couldn't set gpio$GPIO direction"
echo 0 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 100000 2>/dev/null || sleep 1
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 100000 2>/dev/null || sleep 1
echo 0 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 100000 2>/dev/null || sleep 1
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
sleep 1

echo "=== bt-hci-probe: staging firmware (etc/rtl8761bt_fw -> $FW_DIR/rtl8761b_fw, per radxa/rtkbt's RTL8761BTV chip-table entry) ==="
mkdir -p "$FW_DIR"
cp /etc/rtl8761bt_fw "$FW_DIR/rtl8761b_fw"
cp /etc/rtl8761bt_config "$FW_DIR/rtl8761b_config"

echo "=== bt-hci-probe: running rtk_hciattach (foreground, Ctrl-C to stop) ==="
echo "Watch for 'HCI Device Index' or check 'ls /sys/class/bluetooth' in another shell."
"$HCIATTACH" -n -s 1500000 "$TTY" rtk_h5
