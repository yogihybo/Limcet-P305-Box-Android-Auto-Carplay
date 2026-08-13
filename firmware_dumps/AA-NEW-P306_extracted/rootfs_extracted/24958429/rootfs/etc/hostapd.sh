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
insmod /tmp/wlan.ko $1
sleep 0.3
mkdir -p /var/lib/misc/
touch /data/udhcpd.leases
ifconfig wlan0 up
ifconfig wlan0 192.168.43.1 netmask 255.255.255.0 
#echo 0 > /proc/sys/net/ipv4/ip_forward 
#echo 1 > /proc/sys/net/ipv6/conf/wlan0/disable_ipv6
#echo 1 > /proc/sys/net/ipv6/conf/wlan1/disable_ipv6
udhcpd -f /etc/udhcpd.conf wlan0 &
#hostapd -B /etc/hostapd/hostapd.conf
