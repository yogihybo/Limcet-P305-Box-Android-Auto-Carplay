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
# It stays dr_mode="otg" in the DTS for exactly this reason, and
# normally doesn't need a boot-time host/otg toggle here -- the
# kernel's own OTG negotiation (docs/WIRELESS_AND_INIT.md sec 7)
# already handles it dynamically.
#
# Exception: when this boot is a USB-stick test/dev boot
# (root=/dev/sda*, usb0's boot-storage path), we already know for
# certain no wired CarPlay cable is in the picture for this session --
# the whole point of that path is testing off a USB stick, not driving
# a car. Rather than let usb1 waste ~15s trying wired mode first and
# falling back to WiFi only after that times out (docs/WIRELESS_AND_INIT.md
# sec 7's retry-cycling), force it straight to host mode so the
# internal WiFi chip/wlan0 comes up immediately. Any other boot
# (root elsewhere, i.e. actually running in the vehicle) keeps the
# full dynamic otg negotiation, since a real wired cable might
# genuinely be present. See linux-arkmicro's musb_ark.c MUSB_HOST case
# and musb_core.c's mode_store() for why this write alone is enough to
# stick (it also updates otg->state, not just hardware registers, so
# the still-armed OTG poll timer won't try to renegotiate it back).
#
# Sysfs paths: the stock script's /sys/devices/platform/musb-ark1680.N/
# musb-hdrc.N/mode never existed on this kernel at all (a different
# board's platform-device naming) -- our DTS registers these as
# e0100000.usb/e0400000.usb (standard <reg-address>.<node-name> DT
# auto-naming), confirmed against this kernel's own boot-log device
# names.

USB1_MODE_PATH=/sys/devices/platform/e0400000.usb/musb-hdrc.1/mode

root_dev=$(sed -n 's/.*\broot=\([^ ]*\).*/\1/p' /proc/cmdline)

usb1_mode=otg
case "$root_dev" in
	/dev/sda*)
		usb1_mode=host
		echo "switchotg: root is on $root_dev (USB test boot) -- forcing usb1 straight to host mode for fast WiFi, skipping wired-CarPlay negotiation"
		;;
esac

if [ -w "$USB1_MODE_PATH" ]; then
	echo "$usb1_mode" > "$USB1_MODE_PATH"
else
	echo "switchotg: $USB1_MODE_PATH not found/writable, skipping usb1"
fi

#carplay
ifconfig lo up
if [ "$usb1_mode" = otg ]; then
	ifconfig carplay-ncm0 up
	hostname CarPlay
fi
#mdnsd&
