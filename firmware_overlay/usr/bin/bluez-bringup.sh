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

# 2026-08-20: real root cause of AA's RFCOMM channel never getting
# dialed, found via bluetoothd -d (see this file's own comment on that
# flag): src/profile.c:send_new_connection() failed on EVERY phone
# connection attempt ("Android Auto connected to <mac>" immediately
# followed by "sending NewConnection failed", repeating). Traced to
# gdbus/object.c's own explicit check+log ("Unable to send message
# (passing fd blocked?)") -- confirmed this device's stock rootfs
# dbus-daemon (firmware_source/mtd6_rootfs/usr/bin/dbus-daemon) is real
# D-Bus **1.0.2** (strings on its sibling libdbus-1.so.3.2.0 confirms
# the version), and this whole bringup script was invoking plain
# `dbus-daemon` -- resolving via PATH to that ancient stock binary, NOT
# any binary this project actually built. DBUS_TYPE_UNIX_FD /
# NEGOTIATE_UNIX_FD (exactly what Profile1.NewConnection needs to hand
# us a connected socket) wasn't added to D-Bus until 1.3.1 -- a 1.0.2
# daemon has no code path for it at all, which is also the likely real
# explanation for A2DP never actually being audible even after the
# ARK-SDDAC unmute fix (MediaTransport1.Acquire() also hands back a fd
# through this same daemon). Our own cross-built dbus-arm-install
# (AASDK_DEPS_DIR, see custom_ui/Makefile) only ever produced
# libdbus-1.a + headers -- bin/dbus-daemon was never actually linked
# until now (tools/bluetoothd-test/dbus-daemon, built from the same
# vendored dbus-1.14.10 source as bluetoothd's own libdbus-1.a, static,
# NEGOTIATE_UNIX_FD confirmed present via strings). Same
# candidate-path-preference pattern as RTK_HCIATTACH/BLUETOOTHD above.
# Note this one's simpler than those two: tools/bluetoothd-test/
# dbus-daemon is a top-level file in that dir, so a full image rebuild
# (install_diag_tools(), build_bootable_sdcard.sh) copies it straight
# to /usr/bin/dbus-daemon -- DIRECTLY REPLACING the ancient stock
# binary at that exact path, which is the real, permanent fix; plain
# `dbus-daemon` below already resolves correctly once that's happened.
# /data/bluetoothd-test/dbus-daemon is only for the scp-based fast-
# iteration dev workflow (see this repo's own established convention,
# e.g. BLUETOOTHD's identical fallback above) BEFORE a full image
# rebuild has picked up this fix -- checked first so a quick scp of
# just this one binary is enough to test without rebuilding the whole
# SD card image.
if [ -x /data/bluetoothd-test/dbus-daemon ]; then
    DBUS_DAEMON=/data/bluetoothd-test/dbus-daemon
else
    DBUS_DAEMON=dbus-daemon
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
    echo "bluez-bringup: starting system dbus-daemon ($DBUS_DAEMON)"
    "$DBUS_DAEMON" --config-file="$DBUS_SYSTEM_CONF" --nofork >/var/log/dbus-daemon.log 2>&1 &
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
# 2026-08-20: -d added (temporary/diagnostic) -- without it, bluetoothd
# never prints its own src/sdpd-service.c:add_record_to_server() lines,
# so there's been zero real visibility into whether
# BluezAaProfile::register_profile()'s RegisterProfile() call actually
# created a well-formed SDP record for the AA UUID -- only that the
# D-Bus call itself returned without an error, which isn't the same
# thing. Real hardware showed AA still never getting dialed after both
# the CoD (0x240420) and WIFI_INFO_RESPONSE security_mode (5) fixes, so
# this checks the next real unverified layer: the SDP record itself.
# Output still lands in /var/log/bluetoothd.log, NOT custom_ui's own
# console (unlike bt-agent, which is separately re-launched via popen()
# specifically to stream -- this script's own child processes were
# never wired into that path) -- cat that file on the device after a
# test connection attempt. Revert to plain -n once this question is
# answered; -d is chatty (every D-Bus method call, not just SDP).
"$BLUETOOTHD" -n -d >/var/log/bluetoothd.log 2>&1 &
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
# 2026-08-20: Class of Device was 0x240408 (major=Audio/Video, minor=2
# "Hands-free device") -- fine for A2DP/HFP (which only look at major
# class + service class), but real hardware showed AA's own RFCOMM
# profile never getting dialed by the phone even after a fresh re-pair,
# while A2DP connected normally. Decoded per the Bluetooth Core Spec CoD
# layout (bits2-7 = minor device class): 0x240420 keeps the same major
# class/service class bits but sets minor=8 ("Car audio", not
# "Hands-free device") -- the value several real-world Android-Auto-
# wireless dongle projects specifically use so Android's car-detection
# heuristic recognizes this as a car, not just a headset, and offers
# wireless AA setup. Unconfirmed on this hardware yet -- needs a
# real-hardware retest (may also need another fresh re-pair, since CoD
# is typically cached at pairing time same as SDP records).
hciconfig hci0 sspmode 1 2>/dev/null || true
hciconfig hci0 class 0x240420 2>/dev/null || true
hciconfig hci0 auth 2>/dev/null || true
hciconfig hci0 encrypt 2>/dev/null || true
hciconfig hci0 piscan 2>/dev/null || true

# Power on the adapter via D-Bus and enable discoverable/pairable
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true 2>/dev/null || true
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:true 2>/dev/null || true
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:true 2>/dev/null || true
dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Class variant:uint32:2360352 2>/dev/null || true

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
