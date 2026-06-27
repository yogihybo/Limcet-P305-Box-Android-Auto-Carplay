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
    for ko in \
        /lib/modules/3.4.0/wlan_rtl8821cs.ko \
        /lib/modules/3.4.0/wlan_rtl8822cs.ko \
        /lib/modules/3.4.0/wlan_rtl8189fs.ko \
        /lib/modules/3.4.0/wlan_rtl8821cu.ko \
        /lib/modules/3.4.0/wlan_rtl8811cu.ko; do
        [ -f "$ko" ] && insmod "$ko" && break
    done
fi

sleep 1

ifconfig wlan0 up || exit 1
ifconfig wlan0 192.168.43.1 netmask 255.255.255.0
echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6

mkdir -p /var/lib/misc
touch /data/udhcpd.leases
udhcpd /etc/udhcpd.conf &

hostapd -B /etc/hostapd/hostapd.conf
