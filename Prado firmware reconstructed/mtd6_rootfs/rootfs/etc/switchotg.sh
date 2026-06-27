#! /bin/sh
echo otg > /sys/devices/platform/musb-ark1680.0/musb-hdrc.0/mode
echo otg > /sys/devices/platform/musb-ark1680.1/musb-hdrc.1/mode

#carplay
ifconfig lo up
# g_ncm.ko (which creates usb0) registers on musb-hdrc.0, the internal USB bus
# (likely connected to the on-board WiFi/BT module), not the physical rear USB-A port.
# musb-hdrc.1 (physical port) runs g_zero.ko for OTG host negotiation.
# Setting an IP on usb0 here has no effect from the physical port — left disabled.
#ifconfig usb0 192.168.7.1 netmask 255.255.255.0 up
hostname CarPlay
#mdnsd&
