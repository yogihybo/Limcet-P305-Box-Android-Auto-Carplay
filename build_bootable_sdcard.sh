#!/bin/bash
# build_sdcard.sh - Build bootable SD card image for ARK1680 SD boot
#
# Run under Linux or WSL. Requires: parted, dosfstools, e2fsprogs, rsync
#   sudo apt install parted dosfstools e2fsprogs rsync
#
# Usage:
#   ./build_sdcard.sh [options]
#
# Examples:
#   # Build image file (default)
#   ./build_sdcard.sh
#
#   # Build image with custom output name
#   ./build_sdcard.sh --image /tmp/prado_sd.img
#
#   # Write directly to SD card (will prompt for confirmation)
#   ./build_sdcard.sh --device /dev/sdb
#
#   # Custom paths
#   ./build_sdcard.sh --uboot uboot_sdboot.bin --kernel kernel/zImage
#
#   # Skip populating userdata partition (leave blank for first boot)
#   ./build_sdcard.sh --no-userdata
#
#   # Dry run — show commands without executing
#   ./build_sdcard.sh --dry-run

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE=""
DEVICE=""
IMAGE_SIZE_MB=512
P1_SIZE_MB=64           # FAT32 boot partition
P2_SIZE_MB=300          # ext4 rootfs
# p3 gets the remainder

UBOOT_BIN=""            # auto-detected if empty
KERNEL_BIN=""           # auto-detected if empty
ROOTFS_DIR=""           # auto-detected if empty
USERDATA_DIR=""         # auto-detected if empty
SKIP_USERDATA=false
DRY_RUN=false

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'
    exit 0
}

die() { echo "ERROR: $*" >&2; exit 1; }

run() {
    if $DRY_RUN; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image|-i)       IMAGE="$2"; shift 2 ;;
        --device|-d)      DEVICE="$2"; shift 2 ;;
        --size)           IMAGE_SIZE_MB="$2"; shift 2 ;;
        --p1-size)        P1_SIZE_MB="$2"; shift 2 ;;
        --p2-size)        P2_SIZE_MB="$2"; shift 2 ;;
        --uboot)          UBOOT_BIN="$2"; shift 2 ;;
        --kernel)         KERNEL_BIN="$2"; shift 2 ;;
        --rootfs-dir)     ROOTFS_DIR="$2"; shift 2 ;;
        --userdata-dir)   USERDATA_DIR="$2"; shift 2 ;;
        --no-userdata)    SKIP_USERDATA=true; shift ;;
        --dry-run|-n)     DRY_RUN=true; shift ;;
        --help|-h)        usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ---------------------------------------------------------------------------
# Auto-detect paths
# ---------------------------------------------------------------------------

[[ -z "$UBOOT_BIN" ]] && {
    for candidate in \
        "$SCRIPT_DIR/uboot_sdboot.bin" \
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
        "$SCRIPT_DIR/Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin"
    do
        [[ -f "$candidate" ]] && { UBOOT_BIN="$candidate"; break; }
    done
}

[[ -z "$KERNEL_BIN" ]] && {
    for candidate in \
        "$SCRIPT_DIR/kernel/zImage" \
        "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/zImage"
    do
        [[ -f "$candidate" ]] && { KERNEL_BIN="$candidate"; break; }
    done
}

[[ -z "$ROOTFS_DIR" ]] && {
    candidate="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs"
    [[ -d "$candidate" ]] && ROOTFS_DIR="$candidate"
}

[[ -z "$USERDATA_DIR" ]] && {
    candidate="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata"
    [[ -d "$candidate" ]] && USERDATA_DIR="$candidate"
}

# ---------------------------------------------------------------------------
# Validate inputs
# ---------------------------------------------------------------------------

[[ -z "$IMAGE" && -z "$DEVICE" ]] && IMAGE="$SCRIPT_DIR/sd_boot.img"
[[ -n "$IMAGE" && -n "$DEVICE" ]] && die "Specify --image or --device, not both"

[[ -z "$UBOOT_BIN" ]]  && die "UBOOT.BIN not found. Use --uboot or run patch_uboot.py first (outputs uboot_sdboot.bin)"
[[ -z "$KERNEL_BIN" ]] && die "zImage not found. Use --kernel to specify path"
[[ -z "$ROOTFS_DIR" ]] && die "rootfs directory not found. Use --rootfs-dir to specify path"

[[ -f "$UBOOT_BIN" ]]  || die "UBOOT.BIN not found: $UBOOT_BIN"
[[ -f "$KERNEL_BIN" ]] || die "zImage not found: $KERNEL_BIN"
[[ -d "$ROOTFS_DIR" ]] || die "rootfs dir not found: $ROOTFS_DIR"

if ! $SKIP_USERDATA; then
    [[ -z "$USERDATA_DIR" || ! -d "$USERDATA_DIR" ]] && {
        echo "WARNING: userdata directory not found — p3 will be formatted but empty."
        echo "         Use --no-userdata to suppress this warning, or --userdata-dir to specify path."
        SKIP_USERDATA=true
    }
fi

# Check required tools
for tool in parted mkfs.fat mkfs.ext4 losetup rsync; do
    command -v "$tool" &>/dev/null || die "$tool not found. Run: sudo apt install parted dosfstools e2fsprogs rsync"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

P3_START_MB=$((P1_SIZE_MB + P2_SIZE_MB + 1))

echo ""
echo "=== ARK1680 SD Card Builder ==="
echo ""
echo "  Output:      ${IMAGE:-$DEVICE}"
echo "  U-Boot:      $UBOOT_BIN"
echo "  Kernel:      $KERNEL_BIN"
echo "  Rootfs:      $ROOTFS_DIR"
echo "  Userdata:    ${USERDATA_DIR:-<empty>}"
echo ""
echo "  Partition layout:"
echo "    p1: FAT32   1 MiB – ${P1_SIZE_MB} MiB      (UBOOT.BIN + zImage)"
echo "    p2: ext4    $((P1_SIZE_MB+1)) MiB – $((P1_SIZE_MB+P2_SIZE_MB)) MiB    (rootfs)"
echo "    p3: ext4    ${P3_START_MB} MiB – end              (userdata)"
echo ""
$DRY_RUN && echo "  *** DRY RUN — no changes will be made ***"
echo ""

# ---------------------------------------------------------------------------
# Device write safety check
# ---------------------------------------------------------------------------

if [[ -n "$DEVICE" ]]; then
    [[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
    echo "WARNING: This will ERASE all data on $DEVICE"
    read -rp "Type YES to continue: " confirm
    [[ "$confirm" == "YES" ]] || { echo "Aborted."; exit 1; }
    TARGET="$DEVICE"
else
    TARGET="$IMAGE"
fi

# ---------------------------------------------------------------------------
# Create image file
# ---------------------------------------------------------------------------

if [[ -n "$IMAGE" ]]; then
    echo "[1/7] Creating blank image (${IMAGE_SIZE_MB} MB)..."
    run dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_SIZE_MB" status=progress
fi

# ---------------------------------------------------------------------------
# Partition
# ---------------------------------------------------------------------------

echo "[2/7] Partitioning..."
run parted -s "$TARGET" mklabel msdos
run parted -s "$TARGET" mkpart primary fat32  1MiB          "${P1_SIZE_MB}MiB"
run parted -s "$TARGET" mkpart primary ext4   "$((P1_SIZE_MB+1))MiB" "$((P1_SIZE_MB+P2_SIZE_MB))MiB"
run parted -s "$TARGET" mkpart primary ext4   "${P3_START_MB}MiB"    "100%"
run parted -s "$TARGET" set 1 boot on

# ---------------------------------------------------------------------------
# Setup loop device (image only)
# ---------------------------------------------------------------------------

LOOP=""
cleanup() {
    if [[ -n "$LOOP" ]]; then
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3 2>/dev/null || true
        losetup -d "$LOOP" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
    echo "[3/7] Attaching loop device..."
    LOOP=$(losetup -Pf --show "$IMAGE")
    echo "      Loop: $LOOP"
    P1="${LOOP}p1"
    P2="${LOOP}p2"
    P3="${LOOP}p3"
elif [[ -n "$DEVICE" ]]; then
    P1="${DEVICE}1"
    P2="${DEVICE}2"
    P3="${DEVICE}3"
else
    # dry-run
    P1="/dev/loopXp1"
    P2="/dev/loopXp2"
    P3="/dev/loopXp3"
fi

# ---------------------------------------------------------------------------
# Format
# ---------------------------------------------------------------------------

echo "[4/7] Formatting partitions..."
run mkfs.fat -F32 -n BOOT     "$P1"
run mkfs.ext4 -L rootfs   -F  "$P2"
run mkfs.ext4 -L userdata  -F  "$P3"

# ---------------------------------------------------------------------------
# Mount and populate
# ---------------------------------------------------------------------------

if ! $DRY_RUN; then
    mkdir -p /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
    mount "$P1" /tmp/sd_p1
    mount "$P2" /tmp/sd_p2
    mount "$P3" /tmp/sd_p3
fi

echo "[5/7] Populating p1 (boot)..."
run cp "$UBOOT_BIN"  "${DRY_RUN:+/tmp/sd_p1/}${DRY_RUN:-/tmp/sd_p1/}UBOOT.BIN"
run cp "$KERNEL_BIN" "${DRY_RUN:+/tmp/sd_p1/}${DRY_RUN:-/tmp/sd_p1/}zImage"
# initramfs if present
[[ -f "$SCRIPT_DIR/initramfs/initramfs.cpio.gz" ]] && \
    run cp "$SCRIPT_DIR/initramfs/initramfs.cpio.gz" /tmp/sd_p1/

if $DRY_RUN; then
    echo "[dry-run] cp $UBOOT_BIN /tmp/sd_p1/UBOOT.BIN"
    echo "[dry-run] cp $KERNEL_BIN /tmp/sd_p1/zImage"
fi

echo "[6/7] Populating p2 (rootfs)..."
RSYNC_EXCLUDE=(
    --exclude=/proc/
    --exclude=/sys/
    --exclude=/dev/
    --exclude=/tmp/
    --exclude='*.ubifs'
    --exclude='rootfs.img'
    --exclude='ubi.cfg'
)
run rsync -a --info=progress2 "${RSYNC_EXCLUDE[@]}" "$ROOTFS_DIR/" /tmp/sd_p2/

# Recreate empty mount-point directories
if ! $DRY_RUN; then
    mkdir -p /tmp/sd_p2/{proc,sys,dev,tmp}
fi

echo "[7/7] Populating p3 (userdata)..."
if $SKIP_USERDATA; then
    echo "      Skipped — p3 left empty."
elif ! $DRY_RUN; then
    rsync -a --info=progress2 \
        --exclude='*.ubifs' \
        --exclude='userdata.img' \
        "$USERDATA_DIR/" /tmp/sd_p3/
else
    echo "[dry-run] rsync $USERDATA_DIR/ /tmp/sd_p3/"
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------

if ! $DRY_RUN; then
    sync
    umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" && LOOP=""
fi

echo ""
echo "Done."
if [[ -n "$IMAGE" ]]; then
    SIZE=$(du -sh "$IMAGE" 2>/dev/null | cut -f1 || echo "?")
    echo "  Image: $IMAGE  ($SIZE)"
    echo ""
    echo "  Write to SD card:"
    echo "    sudo dd if=$IMAGE of=/dev/sdX bs=4M status=progress && sync"
    echo "  or use Etcher / Raspberry Pi Imager"
fi
echo ""
echo "  Boot sequence:"
echo "    Stepldr loads UBOOT.BIN from p1 → U-Boot loads zImage from p1"
echo "    Kernel mounts p2 as / (ext4) → rcS mounts p3 as /data (ext4)"
