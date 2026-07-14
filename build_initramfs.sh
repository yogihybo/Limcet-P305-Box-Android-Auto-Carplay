#!/usr/bin/env bash
# build_initramfs.sh — minimal initramfs to boot the SD-card ext4 rootfs.
#
# Why this is needed: the ARK1680 kernel (3.4.0) has the MMC *core* and *block*
# layers built in (modules.builtin: mmc_core, mmc_block) but the SD host
# controller driver `ark_dw_mmc.ko` is a loadable *module*, normally insmod'd by
# /etc/all.sh at runtime. So at boot the kernel can't see /dev/mmcblk0p2 and
# `root=/dev/mmcblk0p2` panics with `VFS: Unable to mount root fs on
# unknown-block(0,0)`. This initramfs is mounted with no drivers, insmods that
# one module, then pivots into the real SD rootfs.
#
# busybox on this firmware has no switch_root/pivot_root, so we `chroot` into the
# new root instead (fine for a recovery/test boot). It is dynamically linked
# (glibc 2.27), so its interpreter + libc + libcrypt are bundled.
#
# Load it from U-Boot without needing bootloader initrd support:
#   fatload mmc 0:1 0x2000000 initramfs.cpio.gz
#   setenv bootargs "console=ttyS0,115200n8 mem=180M earlyprintk=serial \
#     ${mtdparts} screen=0 rootwait initrd=0x2000000,0x${filesize}"
#   bootnand
# (bootnand does the arkmicro display handoff and passes bootargs; the kernel's
#  ARM `initrd=addr,size` early-param finds the archive regardless of bootnand
#  not taking an initrd argument. No root= is needed — /init mounts the SD.)
#
# Usage:  sudo bash build_initramfs.sh [ROOTFS_DIR] [OUTPUT.cpio.gz]
# ROOTFS_DIR must be the SAME rootfs going on the SD card, so busybox, its libs,
# and ark_dw_mmc.ko all match each other and the running kernel's vermagic.
set -euo pipefail

ROOTFS_DIR="${1:-Prado firmware reconstructed/mtd6_rootfs/rootfs}"
OUT="${2:-sd_bootable/initramfs.cpio.gz}"
MODULE_REL="lib/modules/3.4.0/kernel/drivers/ark/sdmmc/ark_dw_mmc.ko"

# mknod (device nodes inside the cpio) needs root — same as build_bootable_sdcard.sh
if [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root — mknod needs it (e.g. sudo bash $0)" >&2
    exit 1
fi

BB="$ROOTFS_DIR/bin/busybox"
MOD="$ROOTFS_DIR/$MODULE_REL"
for f in "$BB" "$MOD"; do
    [ -f "$f" ] || { echo "error: missing $f" >&2; exit 1; }
done
for t in cpio gzip mknod; do
    command -v "$t" >/dev/null || { echo "error: '$t' not found (apt install cpio gzip)" >&2; exit 1; }
done

STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT
mkdir -p "$STAGING"/{proc,sys,dev,mnt,bin,sbin,lib}

# --- busybox + applet symlinks -------------------------------------------------
install -m 0755 "$BB" "$STAGING/bin/busybox"
for a in sh mount umount insmod mdev usleep sleep mkdir mknod chroot ls cat echo; do
    ln -sf busybox "$STAGING/bin/$a"
done

# --- dynamic loader + libs busybox needs --------------------------------------
# Resolve each by SONAME, but don't depend on the SONAME symlink existing: a
# Windows checkout drops symlinks (restore_rootfs_symlinks.sh recreates them
# from rootfs.symlinks, but we may run before that). So if the symlink is
# present follow it; otherwise glob the versioned real file (e.g. libc-2.27.so),
# then copy the real file into the initramfs and create the SONAME symlink there.
copy_lib() {
    local soname="$1" base="$2" real=""      # e.g. libc.so.6  libc
    if [ -e "$ROOTFS_DIR/lib/$soname" ]; then
        real="$(basename "$(readlink -f "$ROOTFS_DIR/lib/$soname")")"
    else
        real="$(cd "$ROOTFS_DIR/lib" 2>/dev/null && ls ${base}-*.so 2>/dev/null | head -n1)"
    fi
    if [ -z "$real" ] || [ ! -f "$ROOTFS_DIR/lib/$real" ]; then
        echo "error: cannot resolve $soname in $ROOTFS_DIR/lib (looked for ${base}-*.so)" >&2
        exit 1
    fi
    cp -a "$ROOTFS_DIR/lib/$real" "$STAGING/lib/$real"
    ln -sf "$real" "$STAGING/lib/$soname"
}
copy_lib ld-linux-armhf.so.3 ld
copy_lib libc.so.6           libc
copy_lib libcrypt.so.1       libcrypt

# --- the one missing driver ----------------------------------------------------
install -m 0644 "$MOD" "$STAGING/ark_dw_mmc.ko"

# --- device nodes needed before /dev is populated ------------------------------
mknod -m 0600 "$STAGING/dev/console" c 5 1
mknod -m 0666 "$STAGING/dev/null"    c 1 3

# --- /init ---------------------------------------------------------------------
cat > "$STAGING/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin
mount -t proc     none /proc 2>/dev/null
mount -t sysfs    none /sys  2>/dev/null
mount -t devtmpfs none /dev  2>/dev/null || mdev -s
echo "initramfs: insmod ark_dw_mmc.ko"
insmod /ark_dw_mmc.ko
# wait for the SD partition to enumerate (devtmpfs auto-creates it; mdev otherwise)
i=0
while [ ! -b /dev/mmcblk0p2 ]; do
    mdev -s
    [ -b /dev/mmcblk0p2 ] && break
    i=$((i + 1)); [ "$i" -ge 50 ] && break
    usleep 100000
done
if [ ! -b /dev/mmcblk0p2 ]; then
    echo "initramfs: /dev/mmcblk0p2 not found after insmod — dropping to shell"
    exec sh
fi
echo "initramfs: mounting SD rootfs (/dev/mmcblk0p2)"
if ! mount -t ext4 -o rw /dev/mmcblk0p2 /mnt; then
    echo "initramfs: ext4 mount failed — dropping to shell"
    exec sh
fi
# carry the pseudo-filesystems into the new root
mkdir -p /mnt/proc /mnt/sys /mnt/dev
mount -t proc     none /mnt/proc 2>/dev/null
mount -t sysfs    none /mnt/sys  2>/dev/null
mount -t devtmpfs none /mnt/dev  2>/dev/null
echo "initramfs: chroot into SD rootfs"
for real_init in /sbin/init /init /linuxrc /bin/sh; do
    if [ -x "/mnt$real_init" ]; then
        exec chroot /mnt "$real_init"
    fi
done
echo "initramfs: no init found on SD rootfs — dropping to shell"
exec sh
INIT
chmod 0755 "$STAGING/init"

# --- pack: newc cpio + gzip ----------------------------------------------------
mkdir -p "$(dirname "$OUT")"
( cd "$STAGING" && find . | cpio -o -H newc --quiet | gzip -9 ) > "$OUT"
SZ="$(stat -c%s "$OUT")"
printf 'initramfs written: %s (%s bytes, 0x%x)\n' "$OUT" "$SZ" "$SZ"

# --- wrap as a U-Boot ramdisk image (uInitrd) ----------------------------------
# The uImage header is self-describing (type/size/CRC), so `bootz <kernel>
# <uInitrd> -` and `bootm` consume it without being told a size. The raw
# initramfs.cpio.gz above is kept too: `bootnand` takes no ramdisk argument, so
# that path uses the kernel cmdline `initrd=<addr>,<size>` on the raw archive.
UINITRD="$(dirname "$OUT")/uInitrd"
if command -v mkimage >/dev/null; then
    mkimage -A arm -O linux -T ramdisk -C gzip \
        -n "ARK1680 SD initramfs" -d "$OUT" "$UINITRD" >/dev/null
    printf 'uInitrd written:   %s (%s bytes)\n' "$UINITRD" "$(stat -c%s "$UINITRD")"
else
    echo "warning: mkimage not found (apt install u-boot-tools) — uInitrd not created;" >&2
    echo "         raw $(basename "$OUT") is still usable via initrd=addr,size" >&2
fi

echo ""
echo "Boot options (from p1):"
echo "  A) bootnand + raw cpio.gz  — NAND kernel, does the display handoff:"
echo "       fatload mmc 0:1 0x2000000 $(basename "$OUT")"
echo "       setenv bootargs \"... screen=0 rootwait initrd=0x2000000,0x\${filesize}\" ; bootnand"
echo "  B) bootz + uInitrd         — SD kernel (needs display handoff working):"
echo "       fatload mmc 0:1 0x1000000 zImage ; fatload mmc 0:1 0x2000000 uInitrd"
echo "       bootz 0x1000000 0x2000000 -"
