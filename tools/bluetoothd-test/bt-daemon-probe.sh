#!/bin/sh
# One-shot diagnostic: bring up a real bluetoothd (BlueZ 5.66, this
# tool's own cross-compiled static ARM build) against the kernel's
# hci0 -- see ../rtk-hciattach-test/ for getting hci0 itself up first,
# this tool assumes that already succeeded (hci0 UP RUNNING).
#
# NOT a stack switcher, NOT wired into any UI or the production boot
# path (firmware_overlay/) -- this exists purely to answer whether a
# real BlueZ D-Bus stack works against this device's hci0 at all,
# before anything gets promoted into the production rootfs.
#
# Requires the kernel already have CONFIG_BT_RFCOMM/_TTY,
# CONFIG_BT_BNEP(_MC_FILTER/_PROTO_FILTER), CONFIG_BT_HIDP,
# CONFIG_RFKILL built in -- see hardware/kernel_dot_config, same
# kernel build as ../rtk-hciattach-test/'s CONFIG_BT_HCIUART_3WIRE.
#
# Uses the device's own existing dbus-daemon/libdbus (already on this
# rootfs, /usr/bin/dbus-daemon) for the system bus -- only bluetoothd
# itself is this tool's own build (fully static, see README's "why
# static" section: bluetoothd was built dynamically-linked by default,
# which would require this cross toolchain's newer glibc at runtime;
# relinked -all-static + the project's existing tools/nss-stub wrap
# set, since BlueZ's own glib/libdbus code hits the exact same
# static-NSS crash class documented there).
set -e

SCRIPT_DIR="$(dirname "$0")"
BLUETOOTHD="$SCRIPT_DIR/bluetoothd"
DBUS_POLICY_DST=/usr/etc/dbus-1/system.d
DBUS_SYSTEM_CONF=/usr/etc/dbus-1/system.conf
BUS_SOCKET_DIR=/var/run/dbus
BUS_ADDRESS="unix:path=$BUS_SOCKET_DIR/system_bus_socket"

if pidof bluetoothd >/dev/null 2>&1; then
    echo "bluetoothd is already running -- stop it first (pidof bluetoothd, kill <pid>), then re-run this script."
    exit 1
fi

echo "=== bt-daemon-probe: staging D-Bus policy ($SCRIPT_DIR/dbus-policy/bluetooth.conf -> $DBUS_POLICY_DST/bluetooth.conf) ==="
mkdir -p "$DBUS_POLICY_DST"
cp "$SCRIPT_DIR/dbus-policy/bluetooth.conf" "$DBUS_POLICY_DST/bluetooth.conf"

mkdir -p "$BUS_SOCKET_DIR"

# 2026-08-19: was `pidof dbus-daemon` -- wrong check. /etc/profile
# already starts a SESSION bus (`dbus-launch --auto-syntax`) on every
# login shell, so a dbus-daemon process is essentially always already
# running on this device -- but it's listening on a session socket
# (dbus-launch picks its own autolaunch address, not
# $BUS_SOCKET_DIR/system_bus_socket), not the system bus bluetoothd
# needs. `pidof` can't tell the two apart, so this always skipped
# starting a real system bus daemon. Multiple dbus-daemon processes
# coexist fine (independent sockets) -- check for the actual socket
# file instead of any process by that name.
if [ ! -S "$BUS_SOCKET_DIR/system_bus_socket" ]; then
    echo "=== bt-daemon-probe: starting system dbus-daemon ($BUS_ADDRESS) ==="
    dbus-daemon --config-file="$DBUS_SYSTEM_CONF" --address="$BUS_ADDRESS" --nofork &
    DBUS_PID=$!
    # Give it a moment to create the socket before bluetoothd tries to connect.
    i=0
    while [ ! -S "$BUS_SOCKET_DIR/system_bus_socket" ] && [ $i -lt 50 ]; do
        i=$((i + 1))
        usleep 100000 2>/dev/null || sleep 1
    done
    echo "dbus-daemon pid $DBUS_PID"
else
    echo "=== bt-daemon-probe: system bus socket already exists, reusing it ==="
fi

export DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDRESS"

mkdir -p /var/lib/bluetooth
mkdir -p /usr/lib/bluetooth/plugins   # left empty deliberately -- all plugins built in

echo "=== bt-daemon-probe: running bluetoothd (foreground, -n -d, Ctrl-C to stop) ==="
echo "Watch for 'Bluetooth management interface' / 'Endpoint registered' lines."
echo "From another shell: dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects"
exec "$BLUETOOTHD" -n -d
