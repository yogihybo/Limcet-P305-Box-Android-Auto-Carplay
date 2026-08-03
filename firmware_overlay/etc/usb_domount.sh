#!/bin/sh
# 2026-08-04: USB mass-storage automount, invoked by mdev (see etc/mdev.conf)
# on every add/remove of a sd[a-z][1-9] block device (i.e. USB stick
# partitions -- whole-disk nodes without a partition number are
# deliberately not matched, see mdev.conf's own comment).
#
# Why this exists: testing playback through MsnCoreApp's own built-in media
# player (FactoryConfig.ini's EnableMediaMode, libMusicPlayer.so/
# libVideoPlayer.so) needs USB storage mounted at a fixed path the app
# expects -- confirmed via strings in usr/lib/libMsnCommons.so:
# /media/udisk, /media/udisk2, /media/udisk3 (first/second/third device).
# Nothing in this project's rootfs ever provided this -- mdev previously
# only ran once at boot with no config file at all (etc/mdev.conf didn't
# exist), so USB storage got a device node and nothing else. This is a gap
# this reconstruction never filled, not a regression from a working stock
# mechanism that broke -- stock's original 3.4-kernel rootfs almost
# certainly had its own equivalent hotplug rule that was simply never
# carried over.
#
# mdev sets $MDEV (device basename, e.g. "sda1") and $ACTION ("add" or
# "remove") in the environment before running this script.

MOUNT_BASE="/media/udisk"
DEV="/dev/$MDEV"

# 2026-08-04: guard against auto-mounting the BOOT device's own
# partitions as if they were a separate inserted USB stick. Found on
# real hardware: booting from USB (root=/dev/sdaN) matched this same
# script's own sd[a-z][1-9] mdev.conf rule against every partition of
# the boot stick itself, mounting sda1 (the FAT boot partition) at
# /media/udisk2 and, worse, sda3 (the userdata partition -- already
# mounted at /data by rcS) at /media/udisk, and sda2 (root itself,
# already mounted at /) at /media/udisk3. MsnCoreApp's media player
# would have been browsing live userdata/root content under a "USB
# stick" label, not real test media. ROOTDISK below strips the
# trailing partition-number digit(s) from the kernel's own root=
# bootarg (same parsing rcS already does) to get the whole-disk name
# (e.g. "sda" from "/dev/sda2"), then skips any partition of that same
# disk regardless of which one it is -- this is about the boot MEDIUM,
# not specifically the root partition, since sda1/sda3 aren't root
# either but are just as much "not a separate USB stick" as sda2 is.
ROOTDEV=$(cat /proc/cmdline | sed -n 's/.*root=\([^ ]*\).*/\1/p')
ROOTDISK=$(echo "$ROOTDEV" | sed -n 's#^/dev/\([a-z]*\)[0-9]*$#\1#p')
MDEVDISK=$(echo "$MDEV" | sed -n 's/^\([a-z]*\)[0-9]*$/\1/p')
if [ -n "$ROOTDISK" ] && [ "$MDEVDISK" = "$ROOTDISK" ]; then
	echo "usb_domount: $DEV is part of the boot device ($ROOTDEV on /dev/$ROOTDISK), not a separate USB stick -- skipping"
	exit 0
fi

case "$ACTION" in
add)
	# Find the first free slot: /media/udisk, then /media/udisk2,
	# /media/udisk3 -- matches libMsnCommons.so's own naming exactly
	# (no /media/udisk1, the first slot has no numeric suffix).
	for slot in "" 2 3; do
		target="${MOUNT_BASE}${slot}"
		mkdir -p "$target" 2>/dev/null
		if ! grep -q " $target " /proc/mounts; then
			# Generic mount, no -t: lets the kernel's own
			# superblock probing pick the right registered
			# filesystem (vfat/exfat/ntfs/ext*) rather than
			# hardcoding one and failing on anything else.
			if mount "$DEV" "$target" 2>/dev/null; then
				echo "usb_domount: mounted $DEV at $target"
			else
				echo "usb_domount: failed to mount $DEV at $target"
			fi
			exit 0
		fi
	done
	echo "usb_domount: no free /media/udisk* slot for $DEV (3 already in use?)"
	;;
remove)
	# Find whichever slot this device was actually mounted at --
	# don't assume it's still the first free one, since multiple
	# devices could have come and gone in a different order.
	mountpoint=$(grep "^$DEV " /proc/mounts | awk '{print $2}')
	if [ -n "$mountpoint" ]; then
		umount "$mountpoint" 2>/dev/null && \
			echo "usb_domount: unmounted $DEV from $mountpoint"
	fi
	;;
esac
