#! /bin/sh
# Switch each MUSB controller's operating mode -- overridden here
# (2026-07-19) to fix a wrong sysfs path and a real root-filesystem
# safety hazard in the stock version of this script; updated
# (2026-07-22) once boot-log analysis (docs/WIRELESS_AND_INIT.md sec 7)
# actually confirmed which physical port does what -- corrects a wrong
# assumption the first version of this rewrite made.
#
# usb0 (e0100000.usb) is this board's single external-facing USB port,
# used for booting/testing off a USB storage stick (root=/dev/sda*,
# "bootusb" in U-Boot) and nothing else. linux-arkmicro's ark1668.dtsi
# sets usb0's dr_mode="host" permanently (was "otg") to skip the ID-pin
# OTG negotiation that otherwise cost several seconds of "Cannot
# enable... attempt power cycle" retries at every boot. Because that's
# the DTS boot-time dr_mode (not just a runtime default), musb_core's
# init never calls musb_gadget_setup() for this port -- there is no
# gadget capability to switch back on here even if this script tried,
# so there's nothing for this script to do for usb0 at all.
#
# usb1 (e0400000.usb) is the port that's actually dual-role: it's
# CarPlay's connector, trying wired mode first (the "carplay-ncm"
# gadget, see linux-arkmicro's f_ncm.c) and falling back to the
# internal WiFi chip (host mode) if nothing answers on the wired side.
# It stays dr_mode="otg" in the DTS for exactly this reason and doesn't
# need a boot-time host/otg toggle here -- the kernel's own OTG
# negotiation (docs/WIRELESS_AND_INIT.md sec 7) already handles it
# dynamically. The "echo otg" below is just a defensive nudge in case
# something upstream left it in a non-otg state; normally a no-op.
#
# Sysfs paths: the stock script's /sys/devices/platform/musb-ark1680.N/
# musb-hdrc.N/mode never existed on this kernel at all (a different
# board's platform-device naming) -- our DTS registers these as
# e0100000.usb/e0400000.usb (standard <reg-address>.<node-name> DT
# auto-naming), confirmed against this kernel's own boot-log device
# names.

USB1_MODE_PATH=/sys/devices/platform/e0400000.usb/musb-hdrc.1/mode

if [ -w "$USB1_MODE_PATH" ]; then
	echo otg > "$USB1_MODE_PATH"
else
	echo "switchotg: $USB1_MODE_PATH not found/writable, skipping usb1"
fi

#carplay
ifconfig lo up
ifconfig carplay-ncm0 up
hostname CarPlay
#mdnsd&
