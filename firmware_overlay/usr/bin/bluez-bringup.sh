#!/bin/sh
# Production Bluetooth stack bring-up for custom_ui/androidauto-sidecar --
# real kernel hci0 (rtk_hciattach) + BlueZ (dbus-daemon + bluetoothd),
# replacing blueware for whichever process calls this. NOT used by
# start_msn/MsnCoreApp -- that stock app keeps using blueware directly,
# unchanged. See tools/rtk-hciattach-test/ and tools/bluetoothd-test/
# for the original diagnostic versions of this exact sequence and the
# full hardware-run history behind every step here; this is the
# production form (fixed /usr/share paths instead of $SCRIPT_DIR-
# relative ones, since rtk_hciattach/bluetoothd land directly in
# /usr/bin via install_diag_tools() without their sibling firmware/
# policy subdirectories).
#
# Usage: bluez-bringup.sh start   -- stop blueware, bring up hci0 +
#                                     bluetoothd, run both in the
#                                     background, return once bluetoothd
#                                     is ready (or fail).
#        bluez-bringup.sh stop    -- stop bluetoothd + rtk_hciattach,
#                                     freeing the chip back for blueware.
#        bluez-bringup.sh status  -- show current daemon / interface status.
#
# Callers: bluez_stack.cpp (custom_ui/androidauto-sidecar). Not meant to
# be run interactively, though it's safe to (matches the diagnostic
# tools' own foreground-friendly behavior).
set -e

FW_DIR=/lib/firmware/rtlbt
FW_SRC=/usr/share/bluez-bringup/device-firmware
DBUS_POLICY_DST=/usr/etc/dbus-1/system.d
DBUS_SYSTEM_CONF=/usr/etc/dbus-1/system-diagnostic.conf
DBUS_POLICY_SRC=/usr/share/bluez-bringup/dbus-policy
BUS_SOCKET_DIR=/var/run/run/dbus
BUS_ADDRESS="unix:path=$BUS_SOCKET_DIR/system_bus_socket"
export DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDRESS"
TTY=/dev/ttyHS1
PID_DIR=/var/run/bluez-bringup
RTK_HCIATTACH=/usr/bin/rtk_hciattach
BLUETOOTHD=/usr/bin/bluetoothd

case "$1" in
    start)
        ;;
    stop)
        [ -f "$PID_DIR/bluetoothd.pid" ] && kill "$(cat "$PID_DIR/bluetoothd.pid")" 2>/dev/null || killall -9 bluetoothd 2>/dev/null || true
        [ -f "$PID_DIR/rtk_hciattach.pid" ] && kill "$(cat "$PID_DIR/rtk_hciattach.pid")" 2>/dev/null || killall -9 rtk_hciattach 2>/dev/null || true
        [ -f "$PID_DIR/dbus-daemon.pid" ] && kill "$(cat "$PID_DIR/dbus-daemon.pid")" 2>/dev/null || true
        hciconfig hci0 down 2>/dev/null || true
        rm -rf "$PID_DIR"
        exit 0
        ;;
    status)
        echo "=== BlueZ Status ==="
        pidof dbus-daemon >/dev/null 2>&1 && echo "dbus-daemon: running" || echo "dbus-daemon: stopped"
        pidof rtk_hciattach >/dev/null 2>&1 && echo "rtk_hciattach: running" || echo "rtk_hciattach: stopped"
        pidof bluetoothd >/dev/null 2>&1 && echo "bluetoothd: running" || echo "bluetoothd: stopped"
        [ -e /sys/class/bluetooth/hci0 ] && echo "hci0: attached" || echo "hci0: not attached"
        exit 0
        ;;
    *)
        echo "usage: $0 {start|stop|status}"
        exit 1
        ;;
esac

if pidof bluetoothd >/dev/null 2>&1 && [ -f "$PID_DIR/bluetoothd.pid" ]; then
    echo "bluez-bringup: already running"
    exit 0
fi

mkdir -p "$PID_DIR"
echo "bluez-bringup: stopping old instances and blueware"
killall -9 rtk_hciattach >/dev/null 2>&1 || true
killall blueware >/dev/null 2>&1 || true
i=0
while pidof blueware >/dev/null 2>&1 && [ $i -lt 30 ]; do
    i=$((i + 1))
    usleep 100000 2>/dev/null || sleep 1
done
if pidof blueware >/dev/null 2>&1; then
    echo "bluez-bringup: blueware still running after 3s, giving up"
    exit 1
fi

if [ -x /usr/bin/rtk_hciattach ]; then
    RTK_HCIATTACH=/usr/bin/rtk_hciattach
elif [ -x /data/rtk-hciattach-test/rtk_hciattach ]; then
    RTK_HCIATTACH=/data/rtk-hciattach-test/rtk_hciattach
elif [ -x /data/rtk_hciattach ]; then
    RTK_HCIATTACH=/data/rtk_hciattach
else
    RTK_HCIATTACH=rtk_hciattach
fi

if [ -x /usr/bin/bluetoothd ]; then
    BLUETOOTHD=/usr/bin/bluetoothd
elif [ -x /data/bluetoothd-test/bluetoothd ]; then
    BLUETOOTHD=/data/bluetoothd-test/bluetoothd
else
    BLUETOOTHD=bluetoothd
fi

echo "bluez-bringup: staging firmware"
mkdir -p "$FW_DIR"

if [ -s "$FW_DIR/rtl8761b_fw" ]; then
    echo "bluez-bringup: firmware already present at $FW_DIR/rtl8761b_fw"
else
    FW_FOUND=""
    for p in \
        "$FW_SRC/rtl8761bt_fw" \
        /lib/firmware/rtl8761b_fw \
        /lib/firmware/rtl8761bt_fw \
        /etc/firmware/rtl8761b_fw \
        /usr/lib/firmware/rtl8761b_fw \
        /data/rtl8761bt_fw \
        /data/rtl8761b_fw \
        /data/device-firmware/rtl8761bt_fw \
        /data/device-firmware/rtl8761b_fw \
        /data/rtk-hciattach-test/device-firmware/rtl8761bt_fw \
        /data/rtk-hciattach-test/rtl8761b_fw \
        /data/rtk-hciattach-test/rtl8761bt_fw \
        /etc/rtl8761bt_fw \
        /usr/share/bluez-bringup/device-firmware/rtl8761bt_fw; do
        if [ -s "$p" ]; then
            FW_FOUND="$p"
            break
        fi
    done

    # Dynamic search if not in standard list
    if [ -z "$FW_FOUND" ]; then
        for search_dir in /data /lib /usr /etc; do
            if [ -d "$search_dir" ]; then
                match=$(find "$search_dir" -name "*8761*fw*" -o -name "*8761bt_fw" 2>/dev/null | grep -v "/rtlbt/" | head -n 1)
                if [ -n "$match" ] && [ -s "$match" ]; then
                    FW_FOUND="$match"
                    break
                fi
            fi
        done
    fi

    if [ -n "$FW_FOUND" ]; then
        echo "bluez-bringup: found firmware at $FW_FOUND -> copying to $FW_DIR/rtl8761b_fw"
        cp "$FW_FOUND" "$FW_DIR/rtl8761b_fw"
    else
        echo "bluez-bringup: warning: firmware file rtl8761b_fw not found via search, continuing"
    fi
fi

if [ -s "$FW_DIR/rtl8761b_config" ]; then
    echo "bluez-bringup: config already present at $FW_DIR/rtl8761b_config"
else
    CFG_FOUND=""
    for p in \
        "$FW_SRC/rtl8761bt_config" \
        /lib/firmware/rtl8761b_config \
        /lib/firmware/rtl8761bt_config \
        /etc/firmware/rtl8761b_config \
        /usr/lib/firmware/rtl8761b_config \
        /data/rtl8761bt_config \
        /data/rtl8761b_config \
        /data/device-firmware/rtl8761bt_config \
        /data/device-firmware/rtl8761b_config \
        /data/rtk-hciattach-test/device-firmware/rtl8761bt_config \
        /data/rtk-hciattach-test/rtl8761b_config \
        /data/rtk-hciattach-test/rtl8761bt_config \
        /etc/rtl8761bt_config \
        /usr/share/bluez-bringup/device-firmware/rtl8761bt_config; do
        if [ -s "$p" ]; then
            CFG_FOUND="$p"
            break
        fi
    done

    if [ -z "$CFG_FOUND" ]; then
        for search_dir in /data /lib /usr /etc; do
            if [ -d "$search_dir" ]; then
                match=$(find "$search_dir" -name "*8761*config*" 2>/dev/null | grep -v "/rtlbt/" | head -n 1)
                if [ -n "$match" ] && [ -s "$match" ]; then
                    CFG_FOUND="$match"
                    break
                fi
            fi
        done
    fi

    if [ -n "$CFG_FOUND" ]; then
        echo "bluez-bringup: found config at $CFG_FOUND -> copying to $FW_DIR/rtl8761b_config"
        cp "$CFG_FOUND" "$FW_DIR/rtl8761b_config"
    fi
fi

echo "bluez-bringup: starting rtk_hciattach (hci0)"
"$RTK_HCIATTACH" -n "$TTY" rtk_h5 >/var/log/rtk_hciattach.log 2>&1 &
echo $! > "$PID_DIR/rtk_hciattach.pid"

i=0
while [ ! -e /sys/class/bluetooth/hci0 ] && [ $i -lt 100 ]; do
    i=$((i + 1))
    usleep 100000 2>/dev/null || sleep 1
done
if [ ! -e /sys/class/bluetooth/hci0 ]; then
    echo "bluez-bringup: hci0 did not appear within 10s"
    exit 1
fi

echo "bluez-bringup: staging D-Bus config"
mkdir -p "$DBUS_POLICY_DST"

if [ -f "$DBUS_POLICY_SRC/bluetooth.conf" ]; then
    cp "$DBUS_POLICY_SRC/bluetooth.conf" "$DBUS_POLICY_DST/bluetooth.conf"
elif [ -f /data/bluetoothd-test/dbus-policy/bluetooth.conf ]; then
    cp /data/bluetoothd-test/dbus-policy/bluetooth.conf "$DBUS_POLICY_DST/bluetooth.conf"
elif [ ! -f "$DBUS_POLICY_DST/bluetooth.conf" ]; then
    cat << 'EOF' > "$DBUS_POLICY_DST/bluetooth.conf"
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy user="root">
    <allow own="org.bluez"/>
    <allow send_destination="org.bluez"/>
    <allow send_interface="org.bluez.Agent1"/>
    <allow send_interface="org.bluez.MediaEndpoint1"/>
    <allow send_interface="org.bluez.MediaPlayer1"/>
    <allow send_interface="org.bluez.Profile1"/>
    <allow send_interface="org.bluez.GattCharacteristic1"/>
    <allow send_interface="org.bluez.GattDescriptor1"/>
    <allow send_interface="org.bluez.LEAdvertisement1"/>
    <allow send_interface="org.freedesktop.DBus.ObjectManager"/>
    <allow send_interface="org.freedesktop.DBus.Properties"/>
  </policy>
  <policy context="default">
    <allow send_destination="org.bluez"/>
  </policy>
</busconfig>
EOF
fi

if [ -f "$DBUS_POLICY_SRC/system-diagnostic.conf" ]; then
    cp "$DBUS_POLICY_SRC/system-diagnostic.conf" "$DBUS_SYSTEM_CONF"
elif [ -f /data/bluetoothd-test/dbus-policy/system-diagnostic.conf ]; then
    cp /data/bluetoothd-test/dbus-policy/system-diagnostic.conf "$DBUS_SYSTEM_CONF"
elif [ -f /usr/etc/dbus-1/system.conf ]; then
    sed 's/<user>messagebus<\/user>/<user>root<\/user>/g' /usr/etc/dbus-1/system.conf > "$DBUS_SYSTEM_CONF"
fi

mkdir -p "$BUS_SOCKET_DIR" /var/run/dbus
if [ ! -S "$BUS_SOCKET_DIR/system_bus_socket" ]; then
    echo "bluez-bringup: starting system dbus-daemon"
    dbus-daemon --config-file="$DBUS_SYSTEM_CONF" --nofork >/var/log/dbus-daemon.log 2>&1 &
    echo $! > "$PID_DIR/dbus-daemon.pid"
    i=0
    while [ ! -S "$BUS_SOCKET_DIR/system_bus_socket" ] && [ $i -lt 50 ]; do
        i=$((i + 1))
        usleep 100000 2>/dev/null || sleep 1
    done
    if [ ! -S "$BUS_SOCKET_DIR/system_bus_socket" ]; then
        echo "bluez-bringup: system bus socket did not appear within 5s"
        exit 1
    fi
    ln -sf "$BUS_SOCKET_DIR/system_bus_socket" /var/run/dbus/system_bus_socket 2>/dev/null || true
fi

mkdir -p /var/lib/bluetooth
mkdir -p /usr/var/lib
ln -sf /var/lib/bluetooth /usr/var/lib/bluetooth 2>/dev/null || true
mkdir -p /usr/lib/bluetooth/plugins

echo "bluez-bringup: starting bluetoothd"
"$BLUETOOTHD" -n >/var/log/bluetoothd.log 2>&1 &
echo $! > "$PID_DIR/bluetoothd.pid"

i=0
while ! pidof bluetoothd >/dev/null 2>&1 && [ $i -lt 30 ]; do
    i=$((i + 1))
    usleep 100000 2>/dev/null || sleep 1
done
if ! pidof bluetoothd >/dev/null 2>&1; then
    echo "bluez-bringup: bluetoothd did not start"
    exit 1
fi

echo "bluez-bringup: configuring adapter and auto-pairing agent"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$BUS_SOCKET_DIR/system_bus_socket"
hciconfig hci0 up 2>/dev/null || true
hciconfig hci0 sspmode 1 2>/dev/null || true
hciconfig hci0 piscan 2>/dev/null || true

# Power on the adapter via D-Bus and enable discoverable/pairable
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true 2>/dev/null || true
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:true 2>/dev/null || true
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:true 2>/dev/null || true

# Launch dedicated background auto-pairing agent (NoInputNoOutput)
AGENT_BIN=""
for a in /usr/bin/bt-agent /data/bt-agent /usr/share/bluez-bringup/bt-agent; do
    if [ -x "$a" ]; then
        AGENT_BIN="$a"
        break
    fi
done

if [ -n "$AGENT_BIN" ]; then
    echo "bluez-bringup: starting bt-agent ($AGENT_BIN)"
    "$AGENT_BIN" >/var/log/bt-agent.log 2>&1 &
    echo $! > "$PID_DIR/agent.pid"
fi

echo "bluez-bringup: ready"
exit 0
