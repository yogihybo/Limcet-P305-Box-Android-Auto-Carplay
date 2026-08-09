#!/bin/sh
# Device-side diagnostic for the wireless-AA Bluetooth blocker (see
# docs/IMPLEMENTATION_PLAN.md Phase 2 "Wireless AA" section). Purely
# read-only -- does NOT bring up hci0, does NOT start/stop anything,
# does NOT touch adapter state. Just reports what's already there so
# we know whether it's safe to add our own RFCOMM listener/SDP record
# without colliding with MsnCoreApp's own Bluetooth usage.
#
# Run this on the actual device (not the build host) at a few points:
#   1. Right after boot, before touching the Bluetooth settings screen
#   2. After opening the stock Bluetooth settings / pairing a phone
#      via the stock UI
# and compare the two -- that tells us whether hci0 is boot-time or
# on-demand, and what (if anything) already owns it.

echo "=== hci0 presence ==="
ls -la /sys/class/bluetooth/ 2>&1

echo
echo "=== rfkill state ==="
if [ -e /dev/rfkill ] && command -v rfkill >/dev/null 2>&1; then
    rfkill list 2>&1
else
    echo "(no rfkill binary or /dev/rfkill)"
    ls -la /sys/class/rfkill/ 2>&1
fi

echo
echo "=== recent kernel log lines mentioning bluetooth/hci/rtk ==="
dmesg 2>/dev/null | grep -iE "bluetooth|rtk_btusb|rtkbt|hci_uart|hci0" | tail -60

echo
echo "=== processes possibly touching Bluetooth ==="
ps 2>&1 | grep -iE "msncoreapp|bluetooth|bt|rtk" | grep -v grep

echo
echo "=== dbus-daemon running? (needed for any org.bluez D-Bus path) ==="
ps 2>&1 | grep "[d]bus-daemon"
ls -la /var/run/dbus/ 2>&1

echo
echo "=== any listening RFCOMM/L2CAP sockets already ==="
if command -v netstat >/dev/null 2>&1; then
    netstat -a 2>&1 | grep -i "rfcomm\|l2cap"
else
    echo "(no netstat)"
fi

echo
echo "=== /etc/bluetooth contents ==="
ls -la /etc/bluetooth/ 2>&1

echo
echo "Done. Paste this whole output back."
