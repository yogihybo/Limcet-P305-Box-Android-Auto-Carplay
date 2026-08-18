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

# 2026-08-19: optional second argument -- "h4" to attach using the
# simpler, framing-free Realtek H4 protocol (rtk_h4, same uart[] table
# in src/hciattach.c, shares the same realtek_init()) instead of H5's
# SYNC/CONFIG handshake (rtk_h5, default). Useful for telling apart
# "the wire/baud is fundamentally dead" (H4 would also get nothing)
# from "something specific to H5's own handshake is wrong" (H4 might
# get a response where H5 doesn't). Confirmed real, physical reason
# NOT to suspect a flow-control mismatch either way: this board's
# pinctrl_uart5 group (linux-arkmicro/linux/arch/arm/boot/dts/
# ark1668-pinctrl.dtsi) only mux's RX/TX for this UART -- no RTS/CTS
# pins exist on this board at all, matching UART_FLOWCTL=0.
PROTO="${2:-rtk_h5}"
case "$PROTO" in
    rtk_h5|rtk_h4) ;;
    *)
        echo "usage: $0 [device|reference] [rtk_h5|rtk_h4]  (default: device rtk_h5)"
        exit 1
        ;;
esac

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
# 2026-08-19 REVERTED TO SINGLE PULSE: the double low-then-high cycle
# (added based on blueware's decompiled bpio_reset/bpio_set/bpio_reset/
# bpio_set sequence, then bumped 100ms->150ms based on the real
# RTL8761ATT datasheet's >100ms minimum) has NEVER once succeeded on
# real hardware, cold or warm -- two separate hardware tests both went
# completely silent (zero H5 SYNC responses). The ONLY sequence that
# has ever actually worked on this hardware, twice, is this single
# pulse from this tool's very first commit (661b4c4): one low->high
# cycle at 100ms. Evidence beats theory here -- the decompile-derived
# double-pulse theory was reasonable but is empirically wrong (or at
# least incomplete) for getting the chip to respond at all, whatever
# blueware's own second cycle is actually for. Real EN_CHIP minimum
# (RTL8761ATT datasheet section 3.3.3) is still >100ms, so kept at
# 100ms as the exact value already twice proven to work rather than
# guessing upward again without hardware evidence.
if [ ! -d /sys/class/gpio/gpio$GPIO ]; then
    echo $GPIO > /sys/class/gpio/export 2>/dev/null || true
fi
echo out > /sys/class/gpio/gpio$GPIO/direction 2>/dev/null || echo "warn: couldn't set gpio$GPIO direction"
echo 0 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
usleep 100000 2>/dev/null || sleep 1
echo 1 > /sys/class/gpio/gpio$GPIO/value 2>/dev/null
sleep 1

echo "=== bt-hci-probe: staging firmware ($FW_SOURCE: $FW_SRC_FILE -> $FW_DIR/rtl8761b_fw) ==="
mkdir -p "$FW_DIR"
cp "$FW_SRC_FILE" "$FW_DIR/rtl8761b_fw"
cp "$CFG_SRC_FILE" "$FW_DIR/rtl8761b_config"

echo "=== bt-hci-probe: running rtk_hciattach ($PROTO, foreground, Ctrl-C to stop) ==="
echo "Watch for 'HCI Device Index' or check 'ls /sys/class/bluetooth' in another shell."
"$HCIATTACH" -n -s 1500000 "$TTY" "$PROTO"
