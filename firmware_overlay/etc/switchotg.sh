#! /bin/sh
# Switch each MUSB controller's operating mode -- overridden here
# (2026-07-19) to fix a wrong sysfs path and a real root-filesystem
# safety hazard in the stock version of this script; updated
# (2026-07-22) once boot-log analysis (docs/WIRELESS_AND_INIT.md sec 7)
# actually confirmed which physical port does what -- corrects a wrong
# assumption the first version of this rewrite made.
#
# usb0 (e0100000.usb) is this board's single external-facing USB port
# -- used for booting/testing off a USB storage stick (root=/dev/sda*,
# "bootusb" in U-Boot) AND the wired-CarPlay connector on a real
# vehicle boot (bootmmc/bootsd). Updated 2026-07-27: linux-arkmicro's
# ark1668.dtsi now ships usb0's dr_mode="otg" by default (real
# OTG/gadget capability, needed for wired CarPlay) -- the "host"-mode
# skip of ID-pin negotiation retries is applied per-boot-command
# instead, only for bootusb (which already knows for certain no wired
# CarPlay cable is in the picture, since a boot stick occupies the
# same port): U-Boot's bootusb command `fdt set`s dr_mode="host" on
# the in-RAM DTB right before bootz (see linux-arkmicro's
# ark1668_boot_cmds.c). Because that's still a boot-time dr_mode (not
# a runtime default), musb_core's init still never calls
# musb_gadget_setup() for usb0 specifically on a bootusb boot -- so
# there is genuinely nothing for this script to switch on usb0 on that
# path. On bootmmc/bootsd, usb0 keeps its DTS default (otg) and the
# kernel's own OTG negotiation handles it dynamically, same as usb1
# below -- also nothing for this script to do there, by design.
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
# board's platform-device naming). Confirmed on-device via
# `find /sys/devices/platform -iname mode | grep musb`:
# /sys/devices/platform/ahb/e0400000.usb/musb-hdrc.1/mode -- the DTS's
# "ahb" simple-bus container node (parent of the usb0/usb1 nodes) adds
# an extra path component that a first pass at this script (guessing
# from the DT node names alone, without checking the real device) had
# missed.

USB1_MODE_PATH=/sys/devices/platform/ahb/e0400000.usb/musb-hdrc.1/mode

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
