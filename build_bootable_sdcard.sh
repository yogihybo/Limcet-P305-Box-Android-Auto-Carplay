#!/bin/bash
# build_bootable_sdcard.sh - Interactive bootable SD card builder for ARK1680
#
# Builds an SD card image that boots the kernel and rootfs from removable
# media WITHOUT writing to NAND — see build_update.sh for the (destructive)
# NAND flash update tool instead.
#
# Run under Linux or WSL. Requires: parted, dosfstools, e2fsprogs, rsync, python3
#   sudo apt install parted dosfstools e2fsprogs rsync python3
#
# Usage:
#   ./build_bootable_sdcard.sh [options]
#
# All toggleable options can also be set in the interactive menu; paths and
# sizes are CLI-flag-only.
#
# Options:
#   --image PATH       Output image file path (default: sd_bootable/sd_boot.img)
#   --device PATH      Write directly to block device (e.g. /dev/sdb) instead
#   --size MB          Total image size in MB (default: 512)
#   --uboot PATH       Prebuilt UBOOT.BIN to place on p1 as-is (skips patching)
#   --uboot-src PATH   Raw uboot.bin source — patched via patch_uboot.py, never modified
#   --no-patch-uboot   Use the source uboot.bin as-is without patching
#   --no-patch-nand-offset  Skip redirecting the NAND env offset (not recommended)
#   --root DEVICE      Root device for sdboot bootargs (default: /dev/mmcblk0p2)
#   --kernel PATH      zImage to place on p1
#   --rootfs-dir DIR   Rootfs source directory (mounted as /)
#   --userdata-dir DIR Userdata source directory (mounted as /data)
#   --no-userdata      Leave p3 formatted but empty
#   --no-mtd-redirect  Leave bootlogo/bootanimation/reversingtrack/Unicode
#                      reading from existing NAND data instead of SD
#   --non-interactive  Skip the menu, use defaults/flags only
#   --dry-run          Show commands without executing
#   --help             Show this help

set -euo pipefail

# parted, mkfs.fat, mkfs.ext4, and losetup all install to /usr/sbin or /sbin
# on Debian/Ubuntu, which isn't always on $PATH for non-root shells (WSL,
# non-login shells). Without this, both the requirements check and the
# actual build steps below would report these tools missing even when
# installed.
export PATH="$PATH:/usr/sbin:/sbin"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Colours
# ---------------------------------------------------------------------------
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'

info()    { echo -e "${CYAN}  $*${RESET}"; }
success() { echo -e "${GREEN}  ✔ $*${RESET}"; }
warn()    { echo -e "${YELLOW}  ⚠ $*${RESET}"; }
die()     { echo -e "${RED}ERROR: $*${RESET}" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
OUTPUT_DIR="$SCRIPT_DIR/sd_bootable"

IMAGE=""
DEVICE=""
IMAGE_SIZE_MB=512
P1_SIZE_MB=64
P2_SIZE_MB=300

UBOOT_BIN=""                                  # prebuilt binary (--uboot); used as-is
UBOOT_SRC=""                                  # raw source uboot.bin; patched, never modified
UBOOT_OUT="$OUTPUT_DIR/uboot_sdboot.bin"      # patched output
PATCH_UBOOT=true                              # run patch_uboot.py on the source
PATCH_NAND_OFFSET=true                        # patch_uboot.py --patch-nand-offset
ROOT_DEV="/dev/mmcblk0p2"                     # patch_uboot.py --root (matches p2 rootfs)
KERNEL_BIN=""
ROOTFS_DIR=""
USERDATA_DIR=""
RECONSTRUCTED_DIR=""
SKIP_USERDATA=false
SKIP_MTD_REDIRECT=false
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
        --uboot)           UBOOT_BIN="$2"; PATCH_UBOOT=false; shift 2 ;;
        --uboot-src)       UBOOT_SRC="$2"; shift 2 ;;
        --no-patch-uboot)  PATCH_UBOOT=false; shift ;;
        --no-patch-nand-offset) PATCH_NAND_OFFSET=false; shift ;;
        --root)            ROOT_DEV="$2"; shift 2 ;;
        --kernel)          KERNEL_BIN="$2"; shift 2 ;;
        --rootfs-dir)      ROOTFS_DIR="$2"; shift 2 ;;
        --userdata-dir)    USERDATA_DIR="$2"; shift 2 ;;
        --no-userdata)     SKIP_USERDATA=true; shift ;;
        --no-mtd-redirect) SKIP_MTD_REDIRECT=true; shift ;;
        --non-interactive) NON_INTERACTIVE=true; shift ;;
        --dry-run|-n)      DRY_RUN=true; shift ;;
        --help|-h)         usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

[[ -z "$IMAGE" && -z "$DEVICE" ]] && IMAGE="$OUTPUT_DIR/sd_boot.img"
[[ -n "$IMAGE" && -n "$DEVICE" ]] && die "Specify --image or --device, not both"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
run() {
    if $DRY_RUN; then echo "  [dry-run] $*"; else "$@"; fi
}

# ---------------------------------------------------------------------------
# Auto-detect paths
# ---------------------------------------------------------------------------
autodetect() {
    [[ -z "$RECONSTRUCTED_DIR" ]] && {
        local c="$SCRIPT_DIR/Prado firmware reconstructed"
        [[ -d "$c" ]] && RECONSTRUCTED_DIR="$c"
    }
    # Raw source u-boot — patched into UBOOT_OUT, never modified in place
    [[ -z "$UBOOT_BIN" && -z "$UBOOT_SRC" ]] && {
        for c in \
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd1-mtd2_uboot/uboot.bin" \
            "$SCRIPT_DIR/Prado firmware dump/mtd1-mtd2_uboot/extracted/uboot.bin"
        do [[ -f "$c" ]] && { UBOOT_SRC="$c"; break; }; done
    }
    [[ -z "$KERNEL_BIN" ]] && {
        for c in \
            "$SCRIPT_DIR/Prado firmware reconstructed/mtd5_kernel/zImage" \
            "$SCRIPT_DIR/kernel/zImage"
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
# Requirements — shown once at startup, same pattern as build_update.sh.
# ---------------------------------------------------------------------------
REQUIREMENTS=(
    "parted|partition the image|parted"
    "mkfs.fat|format p1 as FAT32|dosfstools"
    "mkfs.ext4|format p2/p3 as ext4|e2fsprogs"
    "losetup|attach the image as a loop device|util-linux"
    "rsync|copy rootfs/userdata onto the image|rsync"
    "python3|patch U-Boot and rcS|python3"
)

check_requirements() {
    echo -e "${BOLD}  Requirements${RESET}"
    local entry tool desc pkg any_missing=0
    for entry in "${REQUIREMENTS[@]}"; do
        IFS='|' read -r tool desc pkg <<< "$entry"
        if command -v "$tool" &>/dev/null; then
            success "$tool  (${desc})"
        else
            warn "$tool  (${desc}) — not found, install: sudo apt install $pkg"
            any_missing=1
        fi
    done
    echo ""
    if [[ $any_missing -eq 1 ]]; then
        warn "Missing tools will block the build — install them before pressing g."
        $NON_INTERACTIVE || read -rp "  Press Enter to continue..." _
    fi
}

# ---------------------------------------------------------------------------
# Config toggles — the interactive menu's navigable items. Paths and sizes
# stay CLI-flag-only (--uboot-src, --kernel, --rootfs-dir, --userdata-dir,
# --size, --root); the menu only controls these three boolean choices.
# Format: "key|label|description|default"
# ---------------------------------------------------------------------------
CONFIG_ITEMS=(
    "patch_uboot|Patch U-Boot for SD boot|Bakes in sdboot/usbboot compiled-in env and redirects the NAND env offset via patch_uboot.py|ON"
    "patch_nand_offset|Patch NAND env offset redirect|Forces the NAND env CRC to fail so compiled-in sdboot defaults take effect (only applies if Patch U-Boot is on)|ON"
    "include_userdata|Include userdata (p3)|Copies the userdata dir to p3 — if off, p3 is left empty and the app populates /data on first boot|ON"
    "redirect_mtd_data|Redirect bootlogo/bootanimation/etc to SD|Symlinks bootlogo, bootanimation, reversingtrack, and Unicode font (mtd8-11) to files under /nanddata/ on p2 — if off, the device reads these from whatever is already in NAND instead|ON"
)

declare -a CONFIG_SEL
for i in "${!CONFIG_ITEMS[@]}"; do
    IFS='|' read -r _ _ _ default <<< "${CONFIG_ITEMS[$i]}"
    [[ "$default" == "ON" ]] && CONFIG_SEL[$i]=1 || CONFIG_SEL[$i]=0
done
# CLI flags override the menu defaults up front, same as build_update.sh's
# flags override its own PARTITIONS defaults.
$PATCH_UBOOT       || CONFIG_SEL[0]=0
$PATCH_NAND_OFFSET || CONFIG_SEL[1]=0
$SKIP_USERDATA     && CONFIG_SEL[2]=0
$SKIP_MTD_REDIRECT && CONFIG_SEL[3]=0

# ---------------------------------------------------------------------------
# Navigation state — identical pattern to build_update.sh's CURSOR/read_key.
# ---------------------------------------------------------------------------
CURSOR=0

is_current() { [[ $CURSOR -eq $1 ]]; }

toggle_current() {
    [[ ${CONFIG_SEL[$CURSOR]} -eq 1 ]] && CONFIG_SEL[$CURSOR]=0 || CONFIG_SEL[$CURSOR]=1
}

read_key() {
    local key rest
    if ! IFS= read -rsn1 key 2>/dev/null; then
        # read only fails like this on true EOF (stdin closed, no more input
        # ever coming) — a real Enter keypress still reads its \n and
        # returns 0. Signal EOF distinctly so the caller can exit instead of
        # spinning forever redrawing the menu at full speed.
        [[ -z "$key" ]] && { printf '__EOF__'; return; }
    fi
    if [[ "$key" == $'\x1b' ]]; then
        IFS= read -rsn2 -t 0.05 rest 2>/dev/null || true
        key+="$rest"
    fi
    printf '%s' "$key"
}

# ---------------------------------------------------------------------------
# Menu rendering — one line per item, detail line for whichever row is
# highlighted, same layout as build_update.sh's compact menu.
# ---------------------------------------------------------------------------
print_detail() {
    IFS='|' read -r _ label desc _ <<< "${CONFIG_ITEMS[$CURSOR]}"
    echo -e "  ${DIM}${label}:${RESET} ${DIM}$desc${RESET}"
}

print_menu() {
    clear
    echo -e "${CYAN}${BOLD}  ARK1680 Prado — Bootable SD Card Builder${RESET}"
    echo -e "  ${DIM}────────────────────────────────────────────────────────${RESET}"

    echo -e "  ${BOLD}BUILD OPTIONS${RESET}"
    for i in "${!CONFIG_ITEMS[@]}"; do
        IFS='|' read -r key label desc _ <<< "${CONFIG_ITEMS[$i]}"
        local mark
        if [[ ${CONFIG_SEL[$i]} -eq 1 ]]; then
            mark="${GREEN}[X]${RESET}"
        else
            mark="${DIM}[ ]${RESET}"
        fi
        local cursor="  "
        is_current "$i" && cursor="${CYAN}▶ ${RESET}"
        printf "  %b%b  %s\n" "$cursor" "$mark" "$label"
    done

    echo ""
    echo -e "  ${BOLD}SD IMAGE CONTENTS${RESET}  ${DIM}→ ${OUTPUT_DIR#$SCRIPT_DIR/}/$(basename "${IMAGE:-$DEVICE}")${RESET}"
    printf "       ${DIM}%-4s %-22s %s${RESET}\n" "Part" "Item" "File"

    local uboot_status kernel_status rootfs_status userdata_status
    if [[ -n "$UBOOT_BIN" && -f "$UBOOT_BIN" ]] || [[ -n "$UBOOT_SRC" && -f "$UBOOT_SRC" ]]; then
        uboot_status="${GREEN}found${RESET}"
    else
        uboot_status="${RED}missing${RESET}"
    fi
    [[ -n "$KERNEL_BIN" && -f "$KERNEL_BIN" ]] && kernel_status="${GREEN}found${RESET}" || kernel_status="${RED}missing${RESET}"
    [[ -n "$ROOTFS_DIR" && -d "$ROOTFS_DIR" ]] && rootfs_status="${GREEN}found${RESET}" || rootfs_status="${RED}missing${RESET}"
    if [[ ${CONFIG_SEL[2]} -eq 0 ]]; then
        userdata_status="${DIM}skipped — populated on first boot${RESET}"
    elif [[ -n "$USERDATA_DIR" && -d "$USERDATA_DIR" ]]; then
        userdata_status="${GREEN}found${RESET}"
    else
        userdata_status="${RED}missing${RESET}"
    fi

    printf "       p1   %-22s %-16s %b\n" "U-Boot"   "$(basename "${UBOOT_BIN:-${UBOOT_SRC:-uboot.bin}}")" "$uboot_status"
    printf "       p1   %-22s %-16s %b\n" "Kernel"    "$(basename "${KERNEL_BIN:-zImage}")"                "$kernel_status"
    printf "       p2   %-22s %-16s %b\n" "Rootfs"    "$(basename "${ROOTFS_DIR:-rootfs}")"                "$rootfs_status"
    printf "       p3   %-22s %-16s %b\n" "Userdata"  "$(basename "${USERDATA_DIR:-userdata}")"            "$userdata_status"

    echo -e "  ${DIM}────────────────────────────────────────────────────────${RESET}"
    print_detail
    echo -e "  ${BOLD}↑/↓${RESET} move   ${BOLD}Space${RESET}/${BOLD}Enter${RESET} toggle   ${BOLD}a${RESET}/${BOLD}n${RESET} all/none   ${BOLD}g${RESET} go   ${BOLD}q${RESET} quit"
}

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------
validate() {
    if [[ -n "$UBOOT_BIN" ]]; then
        [[ -f "$UBOOT_BIN" ]] || die "UBOOT.BIN not found: $UBOOT_BIN"
    else
        [[ -n "$UBOOT_SRC" ]] || die "No U-Boot source — use --uboot-src or --uboot"
        [[ -f "$UBOOT_SRC" ]] || die "U-Boot source not found: $UBOOT_SRC"
        if [[ ${CONFIG_SEL[0]} -eq 1 ]]; then
            [[ -f "$SCRIPT_DIR/patch_uboot.py" ]] || die "patch_uboot.py not found in $SCRIPT_DIR"
            command -v python3 &>/dev/null || die "python3 not found — needed to patch U-Boot"
        fi
    fi
    [[ -f "$KERNEL_BIN" ]] || die "zImage not found: $KERNEL_BIN"
    [[ -d "$ROOTFS_DIR" ]] || die "rootfs dir not found: $ROOTFS_DIR"
    if [[ ${CONFIG_SEL[2]} -eq 1 && -n "$USERDATA_DIR" ]]; then
        [[ -d "$USERDATA_DIR" ]] || die "userdata dir not found: $USERDATA_DIR"
    fi
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
# Patch U-Boot — patch_uboot.py reads the source and writes UBOOT_OUT.
# The source uboot.bin is never modified; UBOOT_OUT lands in sd_bootable/.
# ---------------------------------------------------------------------------
prepare_uboot() {
    # Explicit prebuilt binary — use as-is.
    if [[ -n "$UBOOT_BIN" ]]; then
        info "U-Boot: using supplied binary $UBOOT_BIN"
        return 0
    fi

    # Patch U-Boot toggled off — use the untouched source directly.
    if [[ ${CONFIG_SEL[0]} -eq 0 ]]; then
        UBOOT_BIN="$UBOOT_SRC"
        warn "U-Boot: using source unpatched — may not boot from SD: $UBOOT_BIN"
        return 0
    fi

    mkdir -p "$OUTPUT_DIR"

    echo ""
    echo -e "${BOLD}  Patching U-Boot for SD boot...${RESET}"
    info "Source: $UBOOT_SRC (unchanged)"
    info "Output: $UBOOT_OUT"
    local nand_flag=""
    [[ ${CONFIG_SEL[1]} -eq 1 ]] && nand_flag="--patch-nand-offset"
    run python3 "$SCRIPT_DIR/patch_uboot.py" \
        -i "$UBOOT_SRC" -o "$UBOOT_OUT" \
        --mode sdboot --root "$ROOT_DEV" \
        $nand_flag

    if ! $DRY_RUN; then
        [[ -f "$UBOOT_OUT" ]] || die "patch_uboot.py did not produce $UBOOT_OUT"
        success "U-Boot patched: $UBOOT_OUT"
    fi
    UBOOT_BIN="$UBOOT_OUT"
}

# ---------------------------------------------------------------------------
# rcS patch — applied to the copy on p2, source tree is never modified
# ---------------------------------------------------------------------------
patch_rcs() {
    local target="$1"
    local redirect_mtd="$2"   # "1" or "0" — CONFIG_SEL[3], redirect_mtd_data
    echo -e "${BOLD}  Patching rcS for SD userdata mount...${RESET}"

    if $DRY_RUN; then
        echo "  [dry-run] patch rcS: replace UBIFS userdata block with SD ext4 first + NAND fallback"
        if [[ "$redirect_mtd" == "1" ]]; then
            echo "  [dry-run] patch rcS: insert /dev/mtdN symlinks to /nanddata/"
        else
            echo "  [dry-run] patch rcS: leave /dev/mtdN pointed at existing NAND data"
        fi
        return
    fi

    [[ -f "$target" ]] || { warn "rcS not found at $target — skipping patch"; return; }

    # Two patches applied via a single Python pass:
    #   1. Replace UBIFS-only /data mount with SD ext4 first + NAND fallback
    #   2. Insert /dev/mtdN symlinks after mdev -s for SD-stored NAND partition
    #      data — skipped if redirect_mtd_data is off, leaving the device
    #      reading these partitions from whatever is already in NAND
    python3 - "$target" "$redirect_mtd" <<'PYEOF'
import sys, re

path = sys.argv[1]
redirect_mtd = sys.argv[2] == "1"
text = open(path).read()

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

MARKER_START = 'USERDATAFS=`cat /proc/mounts | grep ubifs`'

start = text.find(MARKER_START)
end   = text.find('\n#mount /dev/mmcblk0p1 /mnt')

if start == -1 or end == -1:
    print(f"  WARNING: rcS userdata block not found at expected location — patch skipped")
    sys.exit(0)

patched = text[:start] + NEW + '\n' + text[end:]
open(path, 'w').write(patched)
print("  rcS userdata mount block patched for SD boot")

# Patch 2: insert MTD symlink block after /sbin/mdev -s — only if
# redirect_mtd_data is on; otherwise leave /dev/mtdN untouched so the
# device reads bootlogo/bootanimation/reversingtrack/Unicode from NAND.
if not redirect_mtd:
    print("  rcS MTD symlink patch skipped (redirect_mtd_data is off — using existing NAND data)")
else:
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
    mkdir -p "$OUTPUT_DIR"
    local TARGET="${IMAGE:-$DEVICE}"
    local P3_START=$(( P1_SIZE_MB + P2_SIZE_MB + 1 ))
    local do_userdata=0
    [[ ${CONFIG_SEL[2]} -eq 1 && -n "$USERDATA_DIR" ]] && do_userdata=1
    local do_mtd_redirect=${CONFIG_SEL[3]}

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
    patch_rcs /tmp/sd_p2/etc/rc.d/rcS "$do_mtd_redirect"
    if [[ $do_mtd_redirect -eq 1 ]]; then
        populate_nanddata /tmp/sd_p2
    else
        info "Skipping /nanddata/ — bootlogo/bootanimation/reversingtrack/Unicode stay on NAND"
    fi

    # 8. Populate p3 — userdata
    echo -e "${BOLD}[7/7] Populating p3 (userdata)...${RESET}"
    if [[ $do_userdata -eq 0 ]]; then
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
# Non-interactive path — skip the menu, use flags/autodetected values as-is.
# ---------------------------------------------------------------------------
run_non_interactive() {
    check_requirements
    validate
    prepare_uboot
    build
}

# ---------------------------------------------------------------------------
# Main loop — same interaction model as build_update.sh: arrow keys move the
# highlighted row, Space/Enter toggles it, a/n/g/q act immediately.
# ---------------------------------------------------------------------------
run_interactive() {
    clear
    echo -e "${CYAN}${BOLD}  ARK1680 Prado — Bootable SD Card Builder${RESET}"
    echo ""
    check_requirements

    if [[ -n "$DEVICE" ]]; then
        [[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"
    fi

    while true; do
        print_menu
        key=$(read_key)
        [[ "$key" == "__EOF__" ]] && { echo ""; echo "  No more input — exiting."; exit 0; }

        case "$key" in
            $'\x1b[A')  (( CURSOR > 0 )) && CURSOR=$((CURSOR - 1)) ;;
            $'\x1b[B')  (( CURSOR < ${#CONFIG_ITEMS[@]} - 1 )) && CURSOR=$((CURSOR + 1)) ;;
            ''|' ')     toggle_current ;;
            a|A)
                for i in "${!CONFIG_ITEMS[@]}"; do CONFIG_SEL[$i]=1; done
                ;;
            n|N)
                for i in "${!CONFIG_ITEMS[@]}"; do CONFIG_SEL[$i]=0; done
                ;;
            g|G)
                echo ""
                if [[ -n "$DEVICE" ]]; then
                    echo -e "${RED}${BOLD}  WARNING: ALL DATA ON $DEVICE WILL BE ERASED${RESET}"
                    echo ""
                    echo -ne "${BOLD}  Type YES to continue: ${RESET}"
                    local word; read -r word
                    [[ "$word" == "YES" ]] || { echo "Aborted."; continue; }
                fi

                (validate && prepare_uboot && build) || {
                    read -rp "  Press Enter to continue..." _
                    continue
                }

                echo ""
                read -rp "  Press Enter to return to menu, or q to quit: " done_input
                [[ "$done_input" =~ ^[Qq]$ ]] && exit 0
                ;;
            q|Q)
                echo ""
                echo "  Exited."
                exit 0
                ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
autodetect

if $NON_INTERACTIVE; then
    run_non_interactive
else
    run_interactive
fi
