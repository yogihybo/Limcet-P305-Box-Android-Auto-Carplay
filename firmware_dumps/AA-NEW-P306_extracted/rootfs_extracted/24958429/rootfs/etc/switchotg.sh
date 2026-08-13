#! /bin/sh
echo otg > /sys/devices/platform/musb-ark1680.0/musb-hdrc.0/mode
echo otg > /sys/devices/platform/musb-ark1680.1/musb-hdrc.1/mode

#carplay
ifconfig lo up
ifconfig usb0 up
hostname CarPlay
#mdnsd&
