#!/bin/bash
# build_bootable_sdcard.sh - Interactive bootable SD card builder for ARK1680
#
# Run under Linux or WSL. Requires: parted, dosfstools, e2fsprogs, rsync
#   sudo apt install parted dosfstools e2fsprogs rsync
#
# Usage:
#   ./build_bootable_sdcard.sh [options]
#
# All options can also be set interactively at runtime.
#
# Options:
#   --image PATH       Output image file path
#   --device PATH      Write directly to block device (e.g. /dev/sdb)
#   --size MB          Total image size in MB (default: 512)
#   --uboot PATH       UBOOT.BIN to place on p1
#   --kernel PATH      zImage to place on p1
#   --rootfs-dir DIR   Rootfs source directory (mounted as /)
#   --userdata-dir DIR Userdata source directory (mounted as /data)
#   --no-userdata      Leave p3 formatted but empty
#   --non-interactive  Skip all prompts, use defaults/flags only
#   --dry-run          Show commands without executing
#   --help             Show this help

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Colours
# ---------------------------------------------------------------------------
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}  $*${RESET}"; }
success() { echo -e "${GREEN}  ✓ $*${RESET}"; }
warn()    { echo -e "${YELLOW}  ! $*${RESET}"; }
die()     { echo -e "${RED}ERROR: $*${RESET}" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
IMAGE=""
DEVICE=""
IMAGE_SIZE_MB=512
P1_SIZE_MB=64
P2_SIZE_MB=300

UBOOT_BIN=""
KERNEL_BIN=""
ROOTFS_DIR=""
USERDATA_DIR=""
RECONSTRUCTED_DIR=""
SKIP_USERDATA=false
NON_INTERACTIVE=false
DRY_RUN=false

# NAND partition data — keyed by mtd number, value is relative path under RECONSTRUCTED_DIR
# Files are copied to /nanddata/ on p2 and symlinked from /dev/mtdN at boot
declare -A NAND_PARTS=(
    [8]="mtd8_bootlogo/bootlogo"
    [9]="mtd9_bootanimation/bootanimation"
    [10]="mtd10_reversingtrack/reversingtrack"
    [11]="mtd11_unicode/unicode"
)

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image|-i)        IMAGE="$2"; shift 2 ;;
        --device|-d)       DEVICE="$2"; shift 2 ;;
        --size)            IMAGE_SIZE_MB="$2"; shift 2 ;;
        --uboot)           UBOOT_BIN="$2"; shift 2 ;;
        --kernel)          KERNEL_BIN="$2"; shift 2 ;;
        --rootfs-dir)      ROOTFS_DIR="$2"; shift 2 ;;
        --userdata-dir)    USERDATA_DIR="$2"; shift 2 ;;
        --no-userdata)     SKIP_USERDATA=true; shift ;;
        --non-interactive) NON_INTERACTIVE=true; shift ;;
        --dry-run|-n)      DRY_RUN=true; shift ;;
        --help|-h)         usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
run() {
    if $DRY_RUN; then echo "  [dry-run] $*"; else "$@"; fi
}

prompt() {
    local var="$1" question="$2" default="$3"
    if $NON_INTERACTIVE; then eval "$var=\"$default\""; return; fi
    echo -ne "${BOLD}  $question${RESET}"
    [[ -n "$default" ]] && echo -ne " ${CYAN}[$default]${RESET}"
    echo -ne ": "
    local answer; read -r answer
    eval "$var=\"${answer:-$default}\""
}

confirm() {
    if $NON_INTERACTIVE; then return 0; fi
    echo -ne "${BOLD}  $1 ${CYAN}[Y/n]${RESET}: "
    local answer; read -r answer
    [[ -z "$answer" || "$answer" =~ ^[Yy] ]]
}

# ---------------------------------------------------------------------------
# Auto-detect paths
# ---------------------------------------------------------------------------
autodetect() {
    [[ -z "$RECONSTRUCTED_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed"
        [[ -d "$c" ]] && RECONSTRUCTED_DIR="$c"
    }
    [[ -z "$UBOOT_BIN" ]] && {
        for c in \
            "$SCRIPT_DIR/uboot_sdboot.bin" \
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
            "$SCRIPT_DIR/Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin"
        do [[ -f "$c" ]] && { UBOOT_BIN="$c"; break; }; done
    }
    [[ -z "$KERNEL_BIN" ]] && {
        for c in \
            "$SCRIPT_DIR/kernel/zImage" \
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/zImage"
        do [[ -f "$c" ]] && { KERNEL_BIN="$c"; break; }; done
    }
    [[ -z "$ROOTFS_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed/mtd6_rootfs/rootfs"
        [[ -d "$c" ]] && ROOTFS_DIR="$c"
    }
    [[ -z "$USERDATA_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed/mtd7_userdata/userdata"
        [[ -d "$c" ]] && USERDATA_DIR="$c"
    }
    return 0
}

# ---------------------------------------------------------------------------
# Interactive configuration
# ---------------------------------------------------------------------------
configure() {
    echo ""
    echo -e "${BOLD}=== ARK1680 Bootable SD Card Builder ===${RESET}"
    echo ""

    # Output target
    echo -e "${BOLD}  Output target${RESET}"
    if [[ -z "$IMAGE" && -z "$DEVICE" ]]; then
        local target_type
        prompt target_type "Write to (i)mage file or (d)evice?" "i"
        if [[ "$target_type" =~ ^[Dd] ]]; then
            echo ""
            warn "Available block devices:"
            lsblk -dpno NAME,SIZE,MODEL 2>/dev/null | grep -v loop | sed 's/^/    /' || true
            echo ""
            prompt DEVICE "Device path (e.g. /dev/sdb)" ""
            [[ -z "$DEVICE" ]] && die "No device specified"
        else
            prompt IMAGE "Image file path" "$SCRIPT_DIR/sd_boot.img"
        fi
    fi
    [[ -n "$IMAGE" && -n "$DEVICE" ]] && die "Specify --image or --device, not both"

    # Image size
    if [[ -n "$IMAGE" ]]; then
        echo ""
        prompt IMAGE_SIZE_MB "Image size (MB)" "$IMAGE_SIZE_MB"
    fi

    # U-Boot
    echo ""
    echo -e "${BOLD}  Boot files${RESET}"
    if [[ -n "$UBOOT_BIN" ]]; then
        info "U-Boot (auto-detected): $UBOOT_BIN"
        if ! $NON_INTERACTIVE; then
            local alt; prompt alt "Press Enter to accept or enter a different path" ""
            [[ -n "$alt" ]] && UBOOT_BIN="$alt"
        fi
    else
        warn "UBOOT.BIN not found. Run patch_uboot.py first to produce uboot_sdboot.bin:"
        warn "  python patch_uboot.py -i uboot.bin -o uboot_sdboot.bin --mode sdboot --nand-offset-index 0"
        prompt UBOOT_BIN "Path to UBOOT.BIN" ""
        [[ -z "$UBOOT_BIN" ]] && die "UBOOT.BIN is required"
    fi

    # Kernel
    if [[ -n "$KERNEL_BIN" ]]; then
        info "Kernel  (auto-detected): $KERNEL_BIN"
        if ! $NON_INTERACTIVE; then
            local alt; prompt alt "Press Enter to accept or enter a different path" ""
            [[ -n "$alt" ]] && KERNEL_BIN="$alt"
        fi
    else
        warn "zImage not auto-detected."
        prompt KERNEL_BIN "Path to zImage" ""
        [[ -z "$KERNEL_BIN" ]] && die "zImage is required"
    fi

    # Rootfs
    echo ""
    echo -e "${BOLD}  Filesystem sources${RESET}"
    if [[ -n "$ROOTFS_DIR" ]]; then
        info "Rootfs   (auto-detected): $ROOTFS_DIR"
        if ! $NON_INTERACTIVE; then
            local alt; prompt alt "Press Enter to accept or enter a different path" ""
            [[ -n "$alt" ]] && ROOTFS_DIR="$alt"
        fi
    else
        warn "Rootfs directory not auto-detected."
        prompt ROOTFS_DIR "Path to rootfs directory" ""
        [[ -z "$ROOTFS_DIR" ]] && die "Rootfs directory is required"
    fi

    # Userdata
    if ! $SKIP_USERDATA; then
        if [[ -n "$USERDATA_DIR" ]]; then
            info "Userdata (auto-detected): $USERDATA_DIR"
            if ! $NON_INTERACTIVE; then
                local alt; prompt alt "Press Enter to accept or enter a different path" ""
                [[ -n "$alt" ]] && USERDATA_DIR="$alt"
            fi
        else
            warn "Userdata directory not auto-detected."
            if confirm "Leave p3 empty (userdata populated on first boot)?"; then
                SKIP_USERDATA=true
            else
                prompt USERDATA_DIR "Path to userdata directory" ""
                [[ -z "$USERDATA_DIR" ]] && SKIP_USERDATA=true
            fi
        fi
    fi

    # Partition sizes
    echo ""
    echo -e "${BOLD}  Partition layout${RESET}"
    local p3_size=$(( IMAGE_SIZE_MB - P1_SIZE_MB - P2_SIZE_MB - 1 ))
    info "p1 FAT32  ${P1_SIZE_MB} MB   — UBOOT.BIN + zImage"
    info "p2 ext4   ${P2_SIZE_MB} MB   — rootfs (/)"
    info "p3 ext4   ${p3_size} MB   — userdata (/data)"
    if ! $NON_INTERACTIVE; then
        if ! confirm "Accept this layout?"; then
            prompt P1_SIZE_MB "p1 FAT32 size (MB)" "$P1_SIZE_MB"
            prompt P2_SIZE_MB "p2 ext4 rootfs size (MB)" "$P2_SIZE_MB"
            local avail=$(( IMAGE_SIZE_MB - P1_SIZE_MB - P2_SIZE_MB - 1 ))
            info "p3 will use remaining ${avail} MB"
        fi
    fi

    # Summary
    local p3_start=$(( P1_SIZE_MB + P2_SIZE_MB + 1 ))
    local p3_end=$(( IMAGE_SIZE_MB ))
    echo ""
    echo -e "${BOLD}  Summary${RESET}"
    echo ""
    printf "    %-12s %s\n" "Output:"   "${IMAGE:-$DEVICE}"
    [[ -n "$IMAGE" ]] && printf "    %-12s %s MB\n" "Size:" "$IMAGE_SIZE_MB"
    printf "    %-12s %s\n" "U-Boot:"   "$UBOOT_BIN"
    printf "    %-12s %s\n" "Kernel:"   "$KERNEL_BIN"
    printf "    %-12s %s\n" "Rootfs:"   "$ROOTFS_DIR"
    printf "    %-12s %s\n" "Userdata:" "${USERDATA_DIR:-<empty — populated on first boot>}"
    echo ""
    printf "    %-6s %-8s %-20s %s\n" "Part" "Type" "Size" "Contents"
    printf "    %-6s %-8s %-20s %s\n" "p1" "FAT32" "1–${P1_SIZE_MB} MB" "UBOOT.BIN, zImage"
    printf "    %-6s %-8s %-20s %s\n" "p2" "ext4" "$((P1_SIZE_MB+1))–$((P1_SIZE_MB+P2_SIZE_MB)) MB" "rootfs (/)"
    printf "    %-6s %-8s %-20s %s\n" "p3" "ext4" "${p3_start}–${p3_end} MB" "userdata (/data)"
    echo ""
    $DRY_RUN && warn "DRY RUN — no changes will be made"
    echo ""

    if [[ -n "$DEVICE" ]]; then
        [[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
        echo -e "${RED}${BOLD}  WARNING: ALL DATA ON $DEVICE WILL BE ERASED${RESET}"
        echo ""
        if ! $NON_INTERACTIVE; then
            echo -ne "${BOLD}  Type YES to continue: ${RESET}"
            local word; read -r word
            [[ "$word" == "YES" ]] || { echo "Aborted."; exit 1; }
        fi
    else
        confirm "Proceed?" || { echo "Aborted."; exit 1; }
    fi
}

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------
validate() {
    [[ -f "$UBOOT_BIN" ]]  || die "UBOOT.BIN not found: $UBOOT_BIN"
    [[ -f "$KERNEL_BIN" ]] || die "zImage not found: $KERNEL_BIN"
    [[ -d "$ROOTFS_DIR" ]] || die "rootfs dir not found: $ROOTFS_DIR"
    ! $SKIP_USERDATA && [[ -n "$USERDATA_DIR" ]] && \
        { [[ -d "$USERDATA_DIR" ]] || die "userdata dir not found: $USERDATA_DIR"; }
    for tool in parted mkfs.fat mkfs.ext4 losetup rsync; do
        command -v "$tool" &>/dev/null || \
            die "$tool not found — run: sudo apt install parted dosfstools e2fsprogs rsync"
    done
    local avail=$(( IMAGE_SIZE_MB - P1_SIZE_MB - P2_SIZE_MB - 1 ))
    if [[ $avail -lt 32 ]]; then
        die "Only ${avail} MB left for p3 — increase --size or reduce partition sizes"
    fi
}

# ---------------------------------------------------------------------------
# rcS patch — applied to the copy on p2, source tree is never modified
# ---------------------------------------------------------------------------
patch_rcs() {
    local target="$1"
    echo -e "${BOLD}  Patching rcS for SD userdata mount...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] patch rcS: replace UBIFS userdata block with SD ext4 first + NAND fallback"
        return
    fi

    [[ -f "$target" ]] || { warn "rcS not found at $target — skipping patch"; return; }

    # Two patches applied via a single Python pass:
    #   1. Replace UBIFS-only /data mount with SD ext4 first + NAND fallback
    #   2. Insert /dev/mtdN symlinks after mdev -s for SD-stored NAND partition data
    python3 - "$target" <<'PYEOF'
import sys, re

path = sys.argv[1]
text = open(path).read()

OLD = r"""USERDATAFS=`cat /proc/mounts | grep ubifs`
if \[ "\${USERDATAFS}" != "" \]; then
\tmt_partition=7.*?fi
else
\tUSERDATAFS=`cat /proc/mounts | grep yaffs2`
\tif \[ "\${USERDATAFS}" != "" \]; then
\t\tmount -t yaffs2 /dev/mtdblock6 /data/
\tfi
fi"""

NEW = """\
# Mount userdata: SD ext4 (p3) first, NAND UBI fallback, then yaffs2
if mount -o sync -t ext4 /dev/mmcblk0p3 /data 2>/dev/null; then
\techo "userdata: SD ext4 (/dev/mmcblk0p3)"
\tresetenv=$(fw_printenv factory_reset 2>/dev/null || echo "factory_reset=0")
\tif [ "${resetenv##*=}" = "1" ]; then
\t\techo "==============Factory reset, reformat SD userdata!==========="
\t\tumount /data
\t\tmkfs.ext4 -F /dev/mmcblk0p3
\t\tmount -o sync -t ext4 /dev/mmcblk0p3 /data
\t\tfw_setenv factory_reset 0 2>/dev/null || true
\tfi
else
\tUSERDATAFS=`cat /proc/mounts | grep ubifs`
\tif [ "${USERDATAFS}" != "" ]; then
\t\tmtd_partition=7
\t\tresetenv=$(fw_printenv factory_reset 2>/dev/null || echo "factory_reset=0")
\t\tif [ "${resetenv##*=}" = "1" ]; then
\t\t\techo "==============Factory reset, erase data partition!==========="
\t\t\tflash_eraseall /dev/mtd$mtd_partition
\t\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\t\tubimkvol  /dev/ubi1 -N userdata -m 14
\t\t\tubidetach /dev/ubi_ctrl -m $mtd_partition -d 1
\t\t\tfw_setenv factory_reset 0 2>/dev/null || true
\t\tfi
\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\tmount -o sync -t ubifs ubi1_0 /data
\t\tif [ $? -eq 0 ]; then
\t\t\techo "userdata: NAND UBI"
\t\telse
\t\t\tsync; sleep 1
\t\t\tmount -o sync -t ubifs ubi1_0 /data
\t\t\tif [ $? -ne 0 ]; then
\t\t\t\techo "mount failed! then reformat the volume!"
\t\t\t\tubidetach /dev/ubi_ctrl -m $mtd_partition -d 1
\t\t\t\tflash_eraseall /dev/mtd$mtd_partition
\t\t\t\tubiattach /dev/ubi_ctrl -m $mtd_partition
\t\t\t\tubimkvol  /dev/ubi1 -N userdata -m 14
\t\t\t\tmount -o sync -t ubifs ubi1_0 /data
\t\t\t\tsync
\t\t\tfi
\t\tfi
\telse
\t\tUSERDATAFS=`cat /proc/mounts | grep yaffs2`
\t\tif [ "${USERDATAFS}" != "" ]; then
\t\t\tmount -t yaffs2 /dev/mtdblock6 /data/
\t\tfi
\tfi
fi"""

# Use literal string replacement rather than regex — the block is distinctive enough
MARKER_START = 'USERDATAFS=`cat /proc/mounts | grep ubifs`'
MARKER_END   = '\tfi\nfi\n#mount /dev/mmcblk0p1 /mnt'

start = text.find(MARKER_START)
end   = text.find('\n#mount /dev/mmcblk0p1 /mnt')

if start == -1 or end == -1:
    print(f"  WARNING: rcS userdata block not found at expected location — patch skipped")
    sys.exit(0)

patched = text[:start] + NEW + '\n' + text[end:]
open(path, 'w').write(patched)
print("  rcS userdata mount block patched for SD boot")

# Patch 2: insert MTD symlink block after /sbin/mdev -s
text2 = open(path).read()
MDEV_LINE = '/sbin/mdev -s'
mdev_idx = text2.find(MDEV_LINE)
if mdev_idx == -1:
    print("  WARNING: '/sbin/mdev -s' not found in rcS — MTD symlink patch skipped")
else:
    insert_at = mdev_idx + len(MDEV_LINE)
    # Find end of that line
    eol = text2.find('\n', insert_at)
    if eol == -1: eol = len(text2)
    symlink_block = """

# Replace MTD data partition devices with symlinks to SD-stored files.
# SD card is always authoritative for these partitions — any NAND device
# node created by mdev is removed and replaced unconditionally.
for mtdmap in "8:bootlogo" "9:bootanimation" "10:reversingtrack" "11:unicode"; do
\tnum="${mtdmap%%:*}"
\tname="${mtdmap##*:}"
\trm -f /dev/mtd${num}
\tln -sf /nanddata/${name} /dev/mtd${num}
\techo "mtd${num}: /nanddata/${name}"
done"""
    patched2 = text2[:eol] + symlink_block + text2[eol:]
    open(path, 'w').write(patched2)
    print("  rcS MTD symlink block inserted after mdev -s")
PYEOF

    success "rcS patched on p2 (source tree unchanged)"
}

# ---------------------------------------------------------------------------
# NAND partition data — copy to /nanddata/ on p2
# ---------------------------------------------------------------------------
populate_nanddata() {
    local dest="$1/nanddata"
    echo -e "${BOLD}  Populating /nanddata/ (MTD partition data)...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] mkdir /nanddata/ on p2"
        for mtdnum in "${!NAND_PARTS[@]}"; do
            echo "  [dry-run] copy mtd${mtdnum}: ${NAND_PARTS[$mtdnum]}"
        done
        return
    fi

    mkdir -p "$dest"

    for mtdnum in "${!NAND_PARTS[@]}"; do
        local relpath="${NAND_PARTS[$mtdnum]}"
        local filename="${relpath##*/}"
        local src="$RECONSTRUCTED_DIR/$relpath"
        local dst="$dest/$filename"

        if [[ -f "$src" ]] && [[ -s "$src" ]]; then
            cp "$src" "$dst"
            local sz; sz=$(du -sh "$dst" | cut -f1)
            success "mtd${mtdnum} → /nanddata/${filename}  (${sz})"
        else
            # Create empty placeholder — real dump can be dropped in later
            touch "$dst"
            warn "mtd${mtdnum} → /nanddata/${filename}  (placeholder — no dump yet)"
        fi
    done
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
LOOP=""
cleanup() {
    [[ -n "$LOOP" ]] && ! $DRY_RUN && {
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3 2>/dev/null || true
        losetup -d "$LOOP" 2>/dev/null || true
    }
    return 0
}
trap cleanup EXIT

build() {
    local TARGET="${IMAGE:-$DEVICE}"
    local P3_START=$(( P1_SIZE_MB + P2_SIZE_MB + 1 ))

    echo ""
    # 1. Create image file
    if [[ -n "$IMAGE" ]]; then
        echo -e "${BOLD}[1/7] Creating blank image (${IMAGE_SIZE_MB} MB)...${RESET}"
        run dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_SIZE_MB" status=progress
    else
        echo -e "${BOLD}[1/7] Using device $DEVICE${RESET}"
    fi

    # 2. Partition table
    echo -e "${BOLD}[2/7] Partitioning...${RESET}"
    run parted -s "$TARGET" mklabel msdos
    run parted -s "$TARGET" mkpart primary fat32 1MiB "${P1_SIZE_MB}MiB"
    run parted -s "$TARGET" mkpart primary ext4  "$((P1_SIZE_MB+1))MiB" "$((P1_SIZE_MB+P2_SIZE_MB))MiB"
    run parted -s "$TARGET" mkpart primary ext4  "${P3_START}MiB" "100%"
    run parted -s "$TARGET" set 1 boot on

    # 3. Loop device (image files only)
    local P1 P2 P3
    if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
        echo -e "${BOLD}[3/7] Attaching loop device...${RESET}"
        LOOP=$(losetup -Pf --show "$IMAGE")
        info "Loop device: $LOOP"
        P1="${LOOP}p1"; P2="${LOOP}p2"; P3="${LOOP}p3"
    elif [[ -n "$DEVICE" ]]; then
        P1="${DEVICE}1"; P2="${DEVICE}2"; P3="${DEVICE}3"
    else
        P1="/dev/loopXp1"; P2="/dev/loopXp2"; P3="/dev/loopXp3"
    fi

    # 4. Format
    echo -e "${BOLD}[4/7] Formatting partitions...${RESET}"
    run mkfs.fat -F32 -n BOOT    "$P1"
    run mkfs.ext4 -L rootfs   -F "$P2"
    run mkfs.ext4 -L userdata -F "$P3"
    success "p1 FAT32, p2 ext4 (rootfs), p3 ext4 (userdata)"

    # 5. Mount
    if ! $DRY_RUN; then
        mkdir -p /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
        mount "$P1" /tmp/sd_p1
        mount "$P2" /tmp/sd_p2
        mount "$P3" /tmp/sd_p3
    fi

    # 6. Populate p1 — boot files
    echo -e "${BOLD}[5/7] Populating p1 (boot partition)...${RESET}"
    run cp "$UBOOT_BIN"  /tmp/sd_p1/UBOOT.BIN
    run cp "$KERNEL_BIN" /tmp/sd_p1/zImage
    [[ -f "$SCRIPT_DIR/initramfs/initramfs.cpio.gz" ]] && \
        run cp "$SCRIPT_DIR/initramfs/initramfs.cpio.gz" /tmp/sd_p1/
    success "UBOOT.BIN + zImage written to p1"

    # 7. Populate p2 — rootfs
    echo -e "${BOLD}[6/7] Populating p2 (rootfs)...${RESET}"
    run rsync -a --info=progress2 \
        --exclude=/proc/ \
        --exclude=/sys/ \
        --exclude=/dev/ \
        --exclude=/tmp/ \
        --exclude='*.ubifs' \
        --exclude='rootfs.img' \
        --exclude='ubi.cfg' \
        "$ROOTFS_DIR/" /tmp/sd_p2/
    ! $DRY_RUN && mkdir -p /tmp/sd_p2/{proc,sys,dev,tmp}
    success "Rootfs synced to p2"
    patch_rcs /tmp/sd_p2/etc/rc.d/rcS
    populate_nanddata /tmp/sd_p2

    # 8. Populate p3 — userdata
    echo -e "${BOLD}[7/7] Populating p3 (userdata)...${RESET}"
    if $SKIP_USERDATA || [[ -z "$USERDATA_DIR" ]]; then
        warn "Skipped — p3 is empty. App will populate /data on first boot."
    else
        run rsync -a --info=progress2 \
            --exclude='*.ubifs' \
            --exclude='userdata.img' \
            "$USERDATA_DIR/" /tmp/sd_p3/
        success "Userdata synced to p3"
    fi

    # Unmount and detach
    if ! $DRY_RUN; then
        sync
        umount /tmp/sd_p1 /tmp/sd_p2 /tmp/sd_p3
        [[ -n "$LOOP" ]] && { losetup -d "$LOOP"; LOOP=""; }
    fi

    echo ""
    echo -e "${GREEN}${BOLD}=== Build complete ===${RESET}"
    echo ""
    if [[ -n "$IMAGE" ]] && ! $DRY_RUN; then
        local sz; sz=$(du -sh "$IMAGE" | cut -f1)
        success "Image: $IMAGE  ($sz)"
        echo ""
        echo -e "${BOLD}  Write to SD card:${RESET}"
        echo    "    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync"
        echo    "    or use Etcher / Raspberry Pi Imager"
    elif [[ -n "$DEVICE" ]]; then
        success "Written directly to $DEVICE"
    fi
    echo ""
    echo -e "${BOLD}  Boot sequence:${RESET}"
    echo    "    Stepldr  → loads UBOOT.BIN from p1 (FAT32)"
    echo    "    U-Boot   → fatload zImage from p1, sets root=/dev/mmcblk0p2"
    echo    "    Kernel   → mounts p2 ext4 as /"
    echo    "    rcS      → mounts p3 ext4 as /data"
    echo ""
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
autodetect
configure
validate
build
