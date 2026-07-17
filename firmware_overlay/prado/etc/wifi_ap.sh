#!/bin/sh
# Start WiFi access point for SSH management access.
# SSID: carplay_wifi  Password: 88888888  IP: 192.168.43.1

# hostapd needs /dev/urandom for WPA key generation
[ -e /dev/random.orig ] || mv /dev/random /dev/random.orig
ln -sf /dev/urandom /dev/random

# Tune socket buffers (required for stable AP operation)
echo 2097152 > /proc/sys/net/core/rmem_default
echo 2097152 > /proc/sys/net/core/rmem_max
echo 1048576 > /proc/sys/net/core/wmem_default
echo 1048576 > /proc/sys/net/core/wmem_max

# Load WiFi kernel module.
# MsnCoreApp copies the detected chip's driver to /tmp/wlan.ko at runtime.
# At early boot we probe /lib/modules directly — SDIO combo chips first
# (RTL8821CS / RTL8822CS are most likely for Feasycom BT+WiFi modules).
if [ -f /tmp/wlan.ko ]; then
    insmod /tmp/wlan.ko
else
    # Try modprobe first -- uses modules.dep for 4.19.192
    modprobe rtl8821cs 2>/dev/null || \
    modprobe rtl8822cs 2>/dev/null || \
    modprobe rtl8189fs 2>/dev/null || \
    modprobe rtl8821cu 2>/dev/null || \
    modprobe rtl8811cu 2>/dev/null || {
        # Fallback: direct .ko path using running kernel version
        KVER=$(uname -r)
        for ko in \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8821cs/rtl8821cs.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtlwifi/rtl8192cu/rtl8192cu.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8811cu/rtl8811cu.ko \
            /lib/modules/${KVER}/kernel/drivers/net/wireless/realtek/rtl8xxxu/rtl8xxxu.ko; do
            [ -f "$ko" ] && insmod "$ko" && break
        done
    }
fi

# The onboard WiFi module can take a long time to finish USB enumeration
# on this hardware -- the musb controller's own OTG recovery/retry cycle
# (see [musb] messages elsewhere in rcS/dmesg) has been observed taking
# 15-20+ seconds in real boot logs, well past a fixed `sleep 1`. Poll for
# wlan0 to actually exist instead of assuming it's ready.
i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done

ifconfig wlan0 up || exit 1
ifconfig wlan0 192.168.43.1 netmask 255.255.255.0
echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6

mkdir -p /var/lib/misc
touch /data/udhcpd.leases
udhcpd /etc/udhcpd.conf &

hostapd -B /etc/hostapd/hostapd.conf
