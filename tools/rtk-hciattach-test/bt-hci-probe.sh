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
        echo "usage: $0 [device|reference] [rtk_h5|rtk_h4] [gdb]  (default: device rtk_h5)"
        exit 1
        ;;
esac

# 2026-08-19: optional third argument -- "gdb" launches rtk_hciattach
# under gdbserver (tools/gdbserver/, already on $PATH on-device) on
# port 2345 instead of running it directly, so a host gdb-multiarch can
# attach and catch the fc17/fc20 h5_post_hci_cc race live -- static
# reading confirmed the *shape* of the bug reproduces consistently
# (docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md's own investigation) but
# left an unresolved contradiction (h5_post_hci_cc should only be
# reachable when link_estab_state==H5_HCI_RESET, which should not be
# the state at that point per the code's own logic) that only live
# state inspection can settle. See "GDB debugging" section in this
# tool's own README for the full host-side connect instructions.
GDB_MODE="${3:-}"
if [ -n "$GDB_MODE" ] && [ "$GDB_MODE" != "gdb" ]; then
    echo "usage: $0 [device|reference] [rtk_h5|rtk_h4] [gdb]  (default: device rtk_h5)"
    exit 1
fi

FW_DIR=/lib/firmware/rtlbt
TTY=/dev/ttyHS1
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

# 2026-08-19 MOVED INTO rtk_hciattach ITSELF: gpio$GPIO handling used to
# live here as a shell-side pulse before exec'ing rtk_hciattach, but
# that can't replicate blueware's real order -- blueware opens the
# UART *while gpio91 is still low* from its first reset pulse, and
# never closes/reopens that fd through to vendor init. A separate
# shell pulse followed by a fresh process opening its own new fd loses
# that property entirely (and risks the chip emitting something on the
# wire around the pulse that nothing is listening for yet). The GPIO
# dance (first pulse, then -- with the UART already open -- a settle
# delay approximating blueware's real intervening daemon-startup work,
# then the real pulse pair right before vendor init) now lives in
# src/hciattach.c itself (bt_gpio91_init_and_first_pulse()/
# bt_gpio91_settle_and_final_pulse()), called from the SAME process
# that goes on to do H5 sync. See that file's own comment for the full
# reasoning and this tool's README for the investigation history.

echo "=== bt-hci-probe: staging firmware ($FW_SOURCE: $FW_SRC_FILE -> $FW_DIR/rtl8761b_fw) ==="
mkdir -p "$FW_DIR"
cp "$FW_SRC_FILE" "$FW_DIR/rtl8761b_fw"
cp "$CFG_SRC_FILE" "$FW_DIR/rtl8761b_config"

if [ "$GDB_MODE" = "gdb" ]; then
    echo "=== bt-hci-probe: launching rtk_hciattach ($PROTO) under gdbserver :2345 ==="
    echo "On your host: gdb-multiarch $SCRIPT_DIR/rtk_hciattach-debug"
    echo "  (gdb) target remote <device-ip>:2345"
    echo "  (gdb) b h5_post_hci_cc"
    echo "  (gdb) continue"
    echo "See this tool's own README, 'GDB debugging' section, for the full walkthrough."
    exec gdbserver :2345 "$HCIATTACH" -n -s 1500000 "$TTY" "$PROTO"
fi

echo "=== bt-hci-probe: running rtk_hciattach ($PROTO, foreground, Ctrl-C to stop) ==="
echo "Watch for 'HCI Device Index' or check 'ls /sys/class/bluetooth' in another shell."
"$HCIATTACH" -n -s 1500000 "$TTY" "$PROTO"
