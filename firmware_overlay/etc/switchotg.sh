#! /bin/sh
# Switch each MUSB controller's operating mode -- overridden here
# (2026-07-19) to fix a wrong sysfs path and a real root-filesystem
# safety hazard in the stock version of this script; updated
# (2026-07-22, 2026-07-27) as boot-log analysis (docs/1.4_WIRELESS_AND_INIT.md
# sec 7) progressively corrected which physical port does what.
#
# usb0 (e0100000.usb) is this board's single external-facing USB port
# -- used for booting/testing off a USB storage stick (root=/dev/sda*,
# "bootusb" in U-Boot) AND the wired-CarPlay connector on a real
# vehicle boot (bootmmc/bootsd). linux-arkmicro's ark1668.dtsi ships
# usb0's dr_mode="otg" by default (real OTG/gadget capability, needed
# for wired CarPlay); the "host"-mode skip of ID-pin negotiation
# retries is applied per-boot-command instead, only for bootusb (which
# already knows for certain no wired CarPlay cable is in the picture,
# since a boot stick occupies the same port): U-Boot's bootusb command
# `fdt set`s dr_mode="host" on the in-RAM DTB right before bootz (see
# linux-arkmicro's ark1668_boot_cmds.c). There is genuinely nothing for
# this script to switch on usb0 on either path.
#
# usb1 (e0400000.usb) has NO external connector on this board at all
# -- corrected 2026-07-30. It was previously described here (and in
# ark1668.dtsi) as "CarPlay's connector", trying wired mode first and
# falling back to the internal WiFi chip. That was never actually
# observed: every boot log checked shows only the onboard RTL8811CU
# WiFi chip ever enumerating on this bus. Worse, with usb1 also
# "otg", it was WINNING the kernel's gadget-UDC race against usb0 for
# the g_ncm driver (usb_gadget_probe_driver() just binds to "the
# first" UDC with no driver attached, and g_ncm never sets an explicit
# udc_name) -- meaning wired CarPlay's gadget was silently bound to a
# controller with no connector, from boot, on every prior build. Fixed
# at the DTS level: usb1's dr_mode is now permanently "host" (never
# "otg"), removing it from gadget-UDC contention entirely so g_ncm has
# no candidate left except usb0. There is nothing for this script to
# switch on usb1 either now -- it's a fixed DTS default, same as
# usb0's bootusb-only host patch.
#
# What this script still does: decide whether to bring up the
# carplay-ncm0 interface, which now only ever exists when usb0 itself
# is gadget-capable (i.e. NOT a bootusb test boot, where U-Boot forces
# usb0 to host-only and no gadget ever registers at all).

root_dev=$(sed -n 's/.*\broot=\([^ ]*\).*/\1/p' /proc/cmdline)

is_test_boot=0
case "$root_dev" in
	/dev/sda*)
		is_test_boot=1
		echo "switchotg: root is on $root_dev (USB test boot) -- usb0 is host-only this boot, no carplay-ncm0 gadget will register"
		;;
esac

#carplay
ifconfig lo up
if [ "$is_test_boot" = 0 ]; then
	ifconfig carplay-ncm0 up
	hostname CarPlay
fi
#mdnsd&
