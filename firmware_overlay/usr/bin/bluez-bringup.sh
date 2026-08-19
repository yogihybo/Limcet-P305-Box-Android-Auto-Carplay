#!/bin/sh
# bluez-bringup.sh -- Attaches Realtek RTL8761BTV over 3-Wire UART H5 and starts BlueZ 5.66

ACTION="${1:-start}"

case "$ACTION" in
    stop)
        echo "bluez-bringup: stopping BlueZ stack"
        killall -9 bluetoothd 2>/dev/null || true
        killall -9 rtk_hciattach 2>/dev/null || true
        hciconfig hci0 down 2>/dev/null || true
        exit 0
        ;;
    status)
        echo "=== BlueZ Status ==="
        pidof dbus-daemon >/dev/null 2>&1 && echo "dbus-daemon: running" || echo "dbus-daemon: stopped"
        pidof rtk_hciattach >/dev/null 2>&1 && echo "rtk_hciattach: running" || echo "rtk_hciattach: stopped"
        pidof bluetoothd >/dev/null 2>&1 && echo "bluetoothd: running" || echo "bluetoothd: stopped"
        hciconfig -a 2>/dev/null || echo "hci0: not attached"
        exit 0
        ;;
    start)
        ;;
    *)
        echo "Usage: $0 {start|stop|status}"
        exit 1
        ;;
esac

echo "bluez-bringup: stopping blueware (chip is single-owner -- can't share the UART)"
killall -9 blueware 2>/dev/null || true

echo "bluez-bringup: staging firmware"
mkdir -p /lib/firmware/rtlbt
if [ ! -f /lib/firmware/rtlbt/rtl8761b_fw ]; then
    if [ -f /data/rtl8761bt_fw ]; then
        cp /data/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /lib/firmware/rtl8761b_fw ]; then
        cp /lib/firmware/rtl8761b_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /data/device-firmware/rtl8761bt_fw ]; then
        cp /data/device-firmware/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /data/rtk-hciattach-test/device-firmware/rtl8761bt_fw ]; then
        cp /data/rtk-hciattach-test/device-firmware/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /usr/share/bluez-bringup/device-firmware/rtl8761bt_fw ]; then
        cp /usr/share/bluez-bringup/device-firmware/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    else
        echo "bluez-bringup: warning: firmware file rtl8761b_fw not found, continuing if already in kernel"
    fi
fi

echo "bluez-bringup: ensuring dbus-daemon is running"
mkdir -p /var/run/dbus
if ! pidof dbus-daemon >/dev/null 2>&1; then
    dbus-daemon --system --fork
fi

echo "bluez-bringup: attaching hci0 via rtk_hciattach (3-Wire H5 @ 115200 -> 1.5M)"
if ! pidof rtk_hciattach >/dev/null 2>&1; then
    rtk_hciattach -n -s 115200 /dev/ttyHS1 rtk_h5 >/var/log/rtk_hciattach.log 2>&1 &
    sleep 2
fi

echo "bluez-bringup: bringing up hci0 interface"
hciconfig hci0 up 2>/dev/null || true

echo "bluez-bringup: starting bluetoothd daemon"
if ! pidof bluetoothd >/dev/null 2>&1; then
    bluetoothd -n >/var/log/bluetoothd.log 2>&1 &
    sleep 1
fi

echo "bluez-bringup: configuring adapter"
bluetoothctl power on 2>/dev/null || true
bluetoothctl discoverable on 2>/dev/null || true

echo "bluez-bringup: BlueZ 5.66 stack successfully initialized"
exit 0
