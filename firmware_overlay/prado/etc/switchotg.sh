#! /bin/sh
# Switch each MUSB controller's operating mode -- overridden here
# (2026-07-19) to fix a wrong sysfs path and a real root-filesystem
# safety hazard in the stock version of this script.
#
# usb0 (e0100000.usb) is this board's single external-facing USB port,
# shared between two very different uses: booting/testing off a USB
# storage stick (root=/dev/sda*, "bootusb" in U-Boot) and wired
# CarPlay's gadget/OTG mode. linux-arkmicro's ark1668.dtsi now sets
# usb0's dr_mode="host" (was "otg") specifically to skip the ID-pin OTG
# negotiation that otherwise costs several seconds of "Cannot enable...
# attempt power cycle" retries at every boot (see
# docs/WIRELESS_AND_INIT.md) -- confirmed via boot-log analysis that
# the "+Switch peripheral"/"+++Switch OTG+++" cycling is literally this
# same driver's ark_musb_set_mode() being invoked repeatedly by the
# kernel's own automatic negotiation, toggling the ID-pin GPIO back and
# forth until it settles.
#
# That means usb0 now always starts in plain host mode -- fast boot,
# no negotiation -- and THIS script is what re-enables OTG/gadget
# capability afterward for CarPlay. But if the running system's own
# root filesystem is actively mounted from that same port
# (root=/dev/sda*, i.e. we booted off the USB stick this port serves),
# switching it away from host mode here would yank root out from under
# the running system -- a real crash/corruption hazard, not just a
# theoretical one, since this exact combination is used constantly for
# on-device testing this project. Skip the switch in that case; usb0
# just stays in host mode for that boot instead of gaining CarPlay
# capability, which is the only safe choice when it's also serving as
# the boot device.
#
# Sysfs paths: the stock script's /sys/devices/platform/musb-ark1680.N/
# musb-hdrc.N/mode never existed on this kernel at all (a different
# board's platform-device naming) -- our DTS registers these as
# e0100000.usb/e0400000.usb (standard <reg-address>.<node-name> DT
# auto-naming), confirmed against this kernel's own boot-log device
# names.

USB0_MODE_PATH=/sys/devices/platform/e0100000.usb/musb-hdrc.0/mode
USB1_MODE_PATH=/sys/devices/platform/e0400000.usb/musb-hdrc.1/mode

root_dev=$(sed -n 's/.*\broot=\([^ ]*\).*/\1/p' /proc/cmdline)

case "$root_dev" in
	/dev/sda*)
		echo "switchotg: root is on $root_dev (USB) -- leaving usb0 in host mode, not switching to otg"
		;;
	*)
		if [ -w "$USB0_MODE_PATH" ]; then
			echo otg > "$USB0_MODE_PATH"
		else
			echo "switchotg: $USB0_MODE_PATH not found/writable, skipping usb0"
		fi
		;;
esac

if [ -w "$USB1_MODE_PATH" ]; then
	echo otg > "$USB1_MODE_PATH"
else
	echo "switchotg: $USB1_MODE_PATH not found/writable, skipping usb1"
fi

#carplay
ifconfig lo up
ifconfig usb0 up
hostname CarPlay
#mdnsd&
