#!/bin/sh

echo 2097152 > /proc/sys/net/core/rmem_default
echo 2097152 > /proc/sys/net/core/rmem_max
echo 1048576 > /proc/sys/net/core/wmem_default
echo 1048576 > /proc/sys/net/core/wmem_max
echo 0 > /proc/sys/net/ipv4/tcp_timestamps
echo 1 > /proc/sys/net/ipv4/tcp_sack
echo 1 > /proc/sys/net/ipv4/tcp_fack
echo 1 > /proc/sys/net/ipv4/tcp_window_scaling
sysctl -p

mv /dev/random /dev/random.orig
ln -s /dev/urandom /dev/random
#insmod /lib/modules/3.4.0/ark_wlan.ko
# rtw_country_code=US added 2026-08-04: without a valid HW (EFUSE) or SW
# channel plan, rtw_rfctl_decide_init_chplan() falls back to
# RTW_CHPLAN_WORLDWIDE (0x7F), which excludes 5GHz UNII-1 channels
# (36/40/44/48) -- hostapd then fails outright on the dynamic
# hw_mode=a/channel=36 config MsnCoreApp generates for wireless
# CarPlay/Android Auto ("Configured channel (36) not found from the
# channel list of current mode... Could not select hw_mode and
# channel. (-3)"). This driver's old (pre-2025-03-27 SDK) version had
# no channel-plan/regulatory validation at all, so this never
# mattered before. Live-confirmed on hardware: with
# rtw_country_code=US, rtw_rfctl_decide_init_chplan resolves chplan
# 0x1B instead of the 0x7F fallback. $1 (the parameter MsnCoreApp
# itself passes, e.g. "rtw_vht_enable=2") is preserved after it.
insmod /tmp/wlan.ko rtw_country_code=US $1
sleep 0.3
mkdir -p /var/lib/misc/
touch /data/udhcpd.leases
ifconfig wlan0 up
ifconfig wlan0 192.168.43.1 netmask 255.255.255.0
#echo 0 > /proc/sys/net/ipv4/ip_forward
echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6
echo 1 > /proc/sys/net/ipv6/conf/wlan1/disable_ipv6
udhcpd -f /etc/udhcpd.conf wlan0 &
#hostapd -B /etc/hostapd/hostapd.conf
