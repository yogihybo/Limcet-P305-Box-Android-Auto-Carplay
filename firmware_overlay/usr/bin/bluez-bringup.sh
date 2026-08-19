#!/bin/sh
# bluez-bringup.sh -- Attaches Realtek RTL8761BTV over 3-Wire UART H5 and starts BlueZ 5.66

set -e

# 1. Kill legacy blueware daemon if running
killall -9 blueware 2>/dev/null || true

# 2. Stage firmware if needed
if [ ! -f /lib/firmware/rtlbt/rtl8761b_fw ]; then
    mkdir -p /lib/firmware/rtlbt
    if [ -f /data/rtl8761bt_fw ]; then
        cp /data/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /lib/firmware/rtl8761b_fw ]; then
        cp /lib/firmware/rtl8761b_fw /lib/firmware/rtlbt/rtl8761b_fw
    elif [ -f /data/device-firmware/rtl8761bt_fw ]; then
        cp /data/device-firmware/rtl8761bt_fw /lib/firmware/rtlbt/rtl8761b_fw
    fi
fi

# 3. Ensure system D-Bus daemon is running
mkdir -p /var/run/dbus
if ! pidof dbus-daemon >/dev/null 2>&1; then
    dbus-daemon --system --fork
fi

# 4. Attach HCI UART via 3-Wire H5
if ! pidof rtk_hciattach >/dev/null 2>&1; then
    rtk_hciattach -n -s 115200 /dev/ttyHS1 rtk_h5 >/var/log/rtk_hciattach.log 2>&1 &
    sleep 2
fi

# 5. Bring up hci0
hciconfig hci0 up 2>/dev/null || true

# 6. Start BlueZ daemon
if ! pidof bluetoothd >/dev/null 2>&1; then
    bluetoothd -n >/var/log/bluetoothd.log 2>&1 &
    sleep 1
fi

# 7. Set adapter power on and discoverable
bluetoothctl power on 2>/dev/null || true
bluetoothctl discoverable on 2>/dev/null || true

exit 0
