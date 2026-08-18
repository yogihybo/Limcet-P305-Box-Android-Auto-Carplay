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
# 2026-08-19: real hardware evidence -- a fresh power-cycle attempt
# using an active low-then-high "reset" pulse here got ZERO H5 SYNC
# responses at all (totally silent chip), while a later attempt right
# after blueware had already brought the module up once (then killed)
# got three full stages further using this exact same script. The
# daemon's own real, decompile-confirmed enable sequence
# (docs/1.4_WIRELESS_AND_INIT.md:129-134, from libBlueTooth.so/
# blueware's actual documented behavior) is just export+direction=out
# +value=1 -- it NEVER actively pulls this line low first. Matching
# that exactly now instead of inventing our own reset pulse, since the
# invented low pulse is the most likely reason a truly cold chip
# didn't have enough time/the right sequence to power up cleanly.
if [ ! -d /sys/class/gpio/gpio$GPIO ]; then
    echo $GPIO > /sys/class/gpio/export 2>/dev/null || true
fi
echo out > /sys/class/gpio/gpio$GPIO/direction 2>/dev/null || echo "warn: couldn't set gpio$GPIO direction"
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
sleep 1

echo "=== bt-hci-probe: staging firmware (etc/rtl8761bt_fw -> $FW_DIR/rtl8761b_fw, per radxa/rtkbt's RTL8761BTV chip-table entry) ==="
mkdir -p "$FW_DIR"
cp /etc/rtl8761bt_fw "$FW_DIR/rtl8761b_fw"
cp /etc/rtl8761bt_config "$FW_DIR/rtl8761b_config"

echo "=== bt-hci-probe: running rtk_hciattach (foreground, Ctrl-C to stop) ==="
echo "Watch for 'HCI Device Index' or check 'ls /sys/class/bluetooth' in another shell."
"$HCIATTACH" -n -s 1500000 "$TTY" rtk_h5
