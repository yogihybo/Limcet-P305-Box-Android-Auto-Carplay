#!/bin/bash
# build_bootable_sdcard_dyn.sh — standalone bootable SD card builder for the
# dyn rootfs (Buildroot's ark1668_ft_dyn_defconfig + firmware_overlay_dyn/).
# custom_ui/androidauto-sidecar are its current primary workload, not its
# defining scope.
#
# 2026-08-23: genuinely standalone -- ZERO runtime dependency on
# build_bootable_sdcard.sh. Earlier versions of this script called into
# that one (via --rootfs-dir) and then repaired the stock-specific damage
# it left behind afterward (a stale stock symlink-restore manifest
# corrupting this rootfs's real glibc 2.30 symlinks with dangling glibc
# 2.27 references was the worst instance -- a real, boot-blocking bug,
# found and fixed the hard way). A full audit of every step in that
# script's 13-step pipeline found the same class of stock-only coupling
# in several more places (a stock symlink-restore manifest, stock's own
# MTD/NAND-redirect logic and userdata, a stock MsnCoreApp seed-config
# directory) alongside genuinely generic, reusable logic (partition
# layout, loop-device handling, mkfs, U-Boot ARK-header injection). That
# audit is the map this script was built from: every generic piece is
# reimplemented directly here (no dependency, so no future stock-side
# change can leak through again); every stock-only piece is simply
# absent, not disabled via a flag. See merry-snacking-wirth.md for the
# full audit/rewrite history.
#
# Real simplifications this rewrite made possible, verified before
# relying on them (not assumed):
#   - No symlink-restore-from-manifest step: this rootfs's own
#     output/images/rootfs.tar is a real Linux-native Buildroot export
#     with correct native symlinks throughout (confirmed via `tar -tvf`)
#     -- that whole mechanism exists only to recover symlinks a Windows
#     checkout of the STOCK rootfs loses, a problem this rootfs never has.
#   - No busybox-applet-manifest symlink step: Buildroot's own busybox
#     package already ships real symlinks (bin/sh, bin/ls, bin/mount,
#     sbin/mdev -> ../bin/busybox, confirmed via `tar -tvf`) -- same
#     reasoning as above.
#   - No separate kernel-module-install step: output/images/rootfs.tar
#     already contains a complete /lib/modules/4.19.192/ tree with real
#     depmod state (modules.dep/.alias/.symbols, confirmed via `tar -tvf`)
#     via Buildroot's own post_build.sh module-tree copy -- the original
#     script's kernel-module step exists to retrofit modules onto a STOCK
#     rootfs that never shipped with any, not applicable here.
#   - No CRLF conversion: neither this rootfs's tar output nor
#     firmware_overlay_dyn/ has any real CRLF-terminated text file
#     (checked directly, not assumed) -- that step exists for a
#     Windows-editable STOCK rootfs tree, not this one.
#   - No apply_rootfs_perms.sh dependency: `tar -x`, run as real root
#     (this whole script already requires sudo), faithfully restores the
#     exact permissions/ownership recorded in the archive -- which are
#     now genuinely correct at archive-creation time too, since the
#     host-fakeroot chown-faking bug found earlier this session was
#     fixed at its own root cause (fakeroot 1.20.2 -> 1.31).
#
# What's real and kept, straight from p1's own boot-chain necessity (not
# stock-rootfs coupling -- confirmed via the actual U-Boot source,
# u-boot/include/configs/ark1668_limcet_p305.h, not assumed): the stock
# alt-boot files on p1 (uboot_stock.bin/uboot_hybrid.bin for the
# bootstock/boothybrid onboard-NAND recovery chainload commands,
# zImage_stock as bootnand's own NAND-read-failure fallback,
# reversingtrack.raw for U-Boot's own instant reverse-camera preview) --
# these are genuine, rootfs-independent boot-chain features and are
# copied in directly, as a deliberate inclusion, not inherited by
# accident.
#
# Usage:
#   sudo ./build_bootable_sdcard_dyn.sh [options]
#
# Options:
#   --arkmicro-dir DIR          linux-arkmicro repo root (default:
#                                autodetected: sibling dir, then
#                                /home/osboxes/Downloads/linux-arkmicro, then
#                                ~/Downloads/linux-arkmicro of the REAL
#                                invoking user even under sudo)
#   --buildroot-output-dir DIR  Buildroot output/ tree directly (default:
#                                $ARKMICRO_DIR/buildroot/output)
#   --dyn-overlay DIR           firmware_overlay_dyn/ (default: next to
#                                this script)
#   --image PATH                Output image file path (default:
#                                sd_bootable/sd_boot_dyn.img)
#   --size MB                   Total image size in MB (default: 1024)
#   --skip-build                Skip the automatic `make ui androidauto-sidecar`
#                                + re-stage step and deploy whatever's
#                                currently staged in firmware_overlay_dyn/
#                                usr/bin/ as-is (the default behavior exists
#                                specifically so this is never required for
#                                a normal deploy)
#   --dry-run                   Show commands without executing
#   --help                      Show this help
#
# This script always runs non-interactively (no menu -- it only ever
# targets one fixed configuration) and never writes to a real block
# device directly (--image only, a plain file) -- the actual
# `dd ... of=/dev/sdX` write is always a separate manual command, printed
# at the end.
#
# Requires: parted, dosfstools, e2fsprogs, rsync, python3, tar
#   sudo apt install parted dosfstools e2fsprogs rsync python3 tar

set -euo pipefail

export PATH="$PATH:/usr/sbin:/sbin"

# ---------------------------------------------------------------------------
# Colours + status helpers -- same palette/convention as
# build_bootable_sdcard.sh's own non-menu output, kept for visual
# consistency (not sourced -- this script has no other dependency on it).
# ---------------------------------------------------------------------------
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; RESET='\033[0m'
info()    { echo -e "${CYAN}  $*${RESET}"; }
success() { echo -e "${GREEN}  ✔ $*${RESET}"; }
warn()    { echo -e "${YELLOW}  ⚠ $*${RESET}"; }
die()     { echo -e "${RED}ERROR: $*${RESET}" >&2; exit 1; }

STEP_TOTAL=10
STEP_T0=0
begin_step() {
    STEP_T0=$(date +%s)
    echo ""
    echo -e "${CYAN}${BOLD}┌─ [$1/$STEP_TOTAL] $2${RESET}"
}
end_step() {
    local dt=$(( $(date +%s) - STEP_T0 ))
    if [[ "${1:-}" == "skip" ]]; then
        echo -e "${CYAN}└─${RESET} ${DIM}skipped${RESET}"
    else
        echo -e "${CYAN}└─${RESET} ${GREEN}done${RESET} ${DIM}(${dt}s)${RESET}"
    fi
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRY_RUN=false
run() { if $DRY_RUN; then echo -e "${DIM}  [dry-run] $*${RESET}"; else "$@"; fi; }

# ---------------------------------------------------------------------------
# Real invoking user's home, even under sudo -- $HOME is /root under sudo,
# not the real user's home. A real bug hit an earlier version of this
# script that silently pointed at a nonexistent /root/Downloads/... path.
# ---------------------------------------------------------------------------
REAL_HOME=""
if [[ -n "${SUDO_USER:-}" ]]; then
    REAL_HOME="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)"
    [[ -z "$REAL_HOME" ]] && REAL_HOME="$(eval echo ~"$SUDO_USER" 2>/dev/null || true)"
fi
[[ -z "$REAL_HOME" ]] && REAL_HOME="$HOME"

ARKMICRO_DIR=""
for c in \
    "$SCRIPT_DIR/../linux-arkmicro" \
    "/home/osboxes/Downloads/linux-arkmicro" \
    "$REAL_HOME/Downloads/linux-arkmicro"
do [[ -d "$c/buildroot" ]] && { ARKMICRO_DIR="$(cd "$c" && pwd)"; break; }; done
[[ -z "$ARKMICRO_DIR" ]] && ARKMICRO_DIR="$REAL_HOME/Downloads/linux-arkmicro"

BUILDROOT_OUTPUT_DIR=""
DYN_OVERLAY_DIR="$SCRIPT_DIR/firmware_overlay_dyn"
OUTPUT_DIR="$SCRIPT_DIR/sd_bootable"
IMAGE=""
IMAGE_SIZE_MB=1024
P1_SIZE_MB=64
P2_SIZE_MB=512
SKIP_BUILD=false

usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arkmicro-dir)         ARKMICRO_DIR="$2"; shift 2 ;;
        --buildroot-output-dir) BUILDROOT_OUTPUT_DIR="$2"; shift 2 ;;
        --dyn-overlay)          DYN_OVERLAY_DIR="$2"; shift 2 ;;
        --image)                IMAGE="$2"; shift 2 ;;
        --size)                 IMAGE_SIZE_MB="$2"; shift 2 ;;
        --skip-build)           SKIP_BUILD=true; shift ;;
        --dry-run)              DRY_RUN=true; shift ;;
        --help|-h)              usage ;;
        *)                      die "Unknown option: $1 (see --help)" ;;
    esac
done

[[ -z "$BUILDROOT_OUTPUT_DIR" ]] && BUILDROOT_OUTPUT_DIR="$ARKMICRO_DIR/buildroot/output"
[[ -z "$IMAGE" ]] && IMAGE="$OUTPUT_DIR/sd_boot_dyn.img"

ROOTFS_TAR="$BUILDROOT_OUTPUT_DIR/images/rootfs.tar"
KERNEL_BIN="$ARKMICRO_DIR/linux/arch/arm/boot/zImage"
DTB_BIN="$ARKMICRO_DIR/linux/arch/arm/boot/dts/ark1668_limcet_p305.dtb"
UBOOT_SRC="$ARKMICRO_DIR/u-boot/u-boot.bin"
UBOOT_BIN="$UBOOT_SRC"
CUSTOM_UI_DIR="$SCRIPT_DIR/custom_ui"

# p1 boot-chain files that are genuinely load-bearing regardless of which
# rootfs is on p2 -- confirmed via the real U-Boot source, not stock
# leftovers (see this script's own header comment).
BOOTLOGO_RAW="$OUTPUT_DIR/bootlogo.raw"
ARKDATA_INI="$OUTPUT_DIR/arkdata.ini"
STOCK_UBOOT_BIN="$SCRIPT_DIR/firmware_source/mtd1-mtd2_uboot/uboot.bin"
HYBRID_UBOOT_BIN="$OUTPUT_DIR/uboot_hybrid.bin"
REVERSINGTRACK_RAW="$SCRIPT_DIR/firmware_source/mtd10_reversingtrack/reversingtrack"
ZIMAGE_STOCK="$OUTPUT_DIR/zImage_stock"

# ---------------------------------------------------------------------------
# Requirements
# ---------------------------------------------------------------------------
begin_step 1 "Requirements"
REQUIREMENTS=(
    "parted|partition the image|parted"
    "mkfs.fat|format p1 as FAT32|dosfstools"
    "mkfs.ext4|format p2/p3 as ext4|e2fsprogs"
    "losetup|attach the image as a loop device|util-linux"
    "rsync|copy the overlay onto p2|rsync"
    "python3|inject the ARK U-Boot header|python3"
    "tar|extract rootfs.tar onto p2|tar"
)
any_missing=0
for entry in "${REQUIREMENTS[@]}"; do
    IFS='|' read -r tool desc pkg <<< "$entry"
    if command -v "$tool" &>/dev/null; then
        success "$tool  ($desc)"
    else
        warn "$tool  ($desc) — not found, install: sudo apt install $pkg"
        any_missing=1
    fi
done
[[ $any_missing -eq 1 ]] && die "Missing tools — install them and re-run."

[[ -f "$ROOTFS_TAR" ]] || die "rootfs.tar not found: $ROOTFS_TAR\n       Enable BR2_TARGET_ROOTFS_TAR and build ark1668_ft_dyn_defconfig first."
[[ -d "$DYN_OVERLAY_DIR" ]] || die "firmware_overlay_dyn/ not found: $DYN_OVERLAY_DIR"
[[ -f "$KERNEL_BIN" ]] || die "Kernel not found: $KERNEL_BIN\n       Build linux-arkmicro's kernel first."
[[ -f "$DTB_BIN" ]] || die "DTB not found: $DTB_BIN"
[[ -f "$UBOOT_SRC" ]] || die "U-Boot not found: $UBOOT_SRC\n       Build linux-arkmicro's u-boot first."
end_step

# ---------------------------------------------------------------------------
# Auto-build custom_ui/androidauto-sidecar
# ---------------------------------------------------------------------------
begin_step 2 "Building custom_ui + androidauto-sidecar"
if $SKIP_BUILD; then
    warn "--skip-build passed -- deploying whatever's currently staged in $DYN_OVERLAY_DIR/usr/bin/ as-is"
    end_step skip
elif $DRY_RUN; then
    echo -e "${DIM}  [dry-run] make -C $CUSTOM_UI_DIR ui androidauto-sidecar${RESET}"
    end_step
else
    # This build alone is ~300+ compile steps -- logged to a file, not the
    # terminal, with a one-line summary on success and the log's own tail
    # printed inline on failure so a real error is never harder to diagnose.
    info "Building from source (glibc 2.30 / Bootlin GCC 8.4.0 toolchain)..."
    BUILD_LOG="$SCRIPT_DIR/build/custom_ui_build.log"
    mkdir -p "$(dirname "$BUILD_LOG")"
    echo -e "${DIM}    Full compiler output: $BUILD_LOG${RESET}"
    # HOME="$REAL_HOME": custom_ui/Makefile and its third_party/build_*.sh
    # sub-scripts have their own $(HOME)/build-deps-style defaults; under
    # sudo the ambient $HOME is /root, which silently breaks all of them
    # (a real dbus/dbus.h build failure this exact fix closed earlier).
    if ! HOME="$REAL_HOME" BUILDROOT_OUTPUT_DIR="$BUILDROOT_OUTPUT_DIR" make -C "$CUSTOM_UI_DIR" ui androidauto-sidecar > "$BUILD_LOG" 2>&1; then
        echo -e "${RED}ERROR: custom_ui/androidauto-sidecar build failed -- last 40 lines of $BUILD_LOG:${RESET}" >&2
        tail -n 40 "$BUILD_LOG" >&2
        exit 1
    fi
    success "Build succeeded ($(grep -c -- ' -c -o ' "$BUILD_LOG") compile steps) -- full output in $BUILD_LOG"
    info "Re-staging fresh binaries + configs into $DYN_OVERLAY_DIR/usr/bin/"
    cp -f "$CUSTOM_UI_DIR/build/custom_ui" "$CUSTOM_UI_DIR/build/androidauto-sidecar" \
          "$CUSTOM_UI_DIR/build/hal.conf" "$CUSTOM_UI_DIR/build/default_settings.conf" \
          "$DYN_OVERLAY_DIR/usr/bin/"
    rsync -a "$CUSTOM_UI_DIR/build/alsa/" "$DYN_OVERLAY_DIR/usr/bin/alsa/"
    chmod +x "$DYN_OVERLAY_DIR/usr/bin/custom_ui" "$DYN_OVERLAY_DIR/usr/bin/androidauto-sidecar"
    end_step
fi

# ---------------------------------------------------------------------------
# U-Boot: inject the ARK header if not already present, generate uEnv.txt
# ---------------------------------------------------------------------------
begin_step 3 "Preparing U-Boot"
mkdir -p "$OUTPUT_DIR"
ark_header_present() {
    python3 -c "
import struct, sys
with open('$1', 'rb') as f:
    data = f.read(0x40)
sys.exit(0 if len(data) >= 0x40 and struct.unpack_from('<I', data, 0x3c)[0] == 0x12345678 else 1)
" 2>/dev/null
}
if ark_header_present "$UBOOT_BIN"; then
    info "ARK header already present in $UBOOT_BIN — using as-is"
else
    info "No ARK header found — injecting via inject_ark_header.py..."
    run python3 "$SCRIPT_DIR/build_tools/inject_ark_header.py" "$UBOOT_BIN" "$OUTPUT_DIR/UBOOT.BIN"
    UBOOT_BIN="$OUTPUT_DIR/UBOOT.BIN"
fi
UENV_OUT="$OUTPUT_DIR/uEnv.txt"
if $DRY_RUN; then
    echo -e "${DIM}  [dry-run] write $UENV_OUT${RESET}"
else
    cat > "$UENV_OUT" <<EOF
bootargs=console=ttyS0,115200n8 mem=180M earlyprintk=serial root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw screen=0 user_debug=8
bootcmd=fatload mmc 0:1 0x1000000 zImage; fatload mmc 0:1 0x2000000 ark1668_limcet_p305.dtb; bootz 0x1000000 - 0x2000000
EOF
fi
success "U-Boot: $UBOOT_BIN, uEnv.txt: $UENV_OUT"
end_step

# ---------------------------------------------------------------------------
# Image + partition table + loop device
# ---------------------------------------------------------------------------
begin_step 4 "Creating blank image (${IMAGE_SIZE_MB} MB) + partition table"
run dd if=/dev/zero of="$IMAGE" bs=1M count="$IMAGE_SIZE_MB" status=progress
# Zero the ~1MiB gap before p1, where U-Boot's own environment lives
# (CONFIG_ENV_IS_IN_MMC) -- without this, a stale env from an earlier
# build on the same reused image could carry forward across a "fresh"
# rebuild.
run dd if=/dev/zero of="$IMAGE" bs=1M count=1 conv=notrunc
P3_START=$(( P1_SIZE_MB + P2_SIZE_MB + 1 ))
run parted -s "$IMAGE" mklabel msdos
run parted -s "$IMAGE" mkpart primary fat32 1MiB "${P1_SIZE_MB}MiB"
run parted -s "$IMAGE" mkpart primary ext4 "$((P1_SIZE_MB+1))MiB" "$((P1_SIZE_MB+P2_SIZE_MB))MiB"
run parted -s "$IMAGE" mkpart primary ext4 "${P3_START}MiB" "100%"
run parted -s "$IMAGE" set 1 boot on
success "p1 (BOOT), p2 (ROOTFS), p3 (USERDATA) written to partition table"
end_step

begin_step 5 "Attaching loop device + formatting"
LOOP=""; MNT1=""; MNT2=""; MNT3=""
cleanup() {
    [[ -n "$MNT1" ]] && { mountpoint -q "$MNT1" 2>/dev/null && umount "$MNT1" 2>/dev/null; rmdir "$MNT1" 2>/dev/null; }
    [[ -n "$MNT2" ]] && { mountpoint -q "$MNT2" 2>/dev/null && umount "$MNT2" 2>/dev/null; rmdir "$MNT2" 2>/dev/null; }
    [[ -n "$MNT3" ]] && { mountpoint -q "$MNT3" 2>/dev/null && umount "$MNT3" 2>/dev/null; rmdir "$MNT3" 2>/dev/null; }
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT
if ! $DRY_RUN; then
    LOOP=$(losetup -Pf --show "$IMAGE")
    P1="${LOOP}p1"; P2="${LOOP}p2"; P3="${LOOP}p3"
    info "Loop device: $LOOP"
else
    P1="/dev/loopXp1"; P2="/dev/loopXp2"; P3="/dev/loopXp3"
fi
# 3.4-era kernel/U-Boot ext4 drivers predate the 64bit/metadata_csum
# features modern e2fsprogs enables by default -- strip them so both can
# mount p2/p3.
EXT4_COMPAT="^64bit,^metadata_csum"
run mkfs.fat -F32 -n BOOT "$P1"
run mkfs.ext4 -O "$EXT4_COMPAT" -L rootfs -F "$P2"
run mkfs.ext4 -O "$EXT4_COMPAT" -L userdata -F "$P3"
success "p1 FAT32, p2/p3 ext4 formatted"
end_step

# ---------------------------------------------------------------------------
# Mount
# ---------------------------------------------------------------------------
begin_step 6 "Mounting partitions"
if ! $DRY_RUN; then
    MNT1=$(mktemp -d); MNT2=$(mktemp -d); MNT3=$(mktemp -d)
    mount "$P1" "$MNT1"
    mount "$P2" "$MNT2"
    mount "$P3" "$MNT3"
    success "p1 → $MNT1, p2 → $MNT2, p3 → $MNT3"
else
    MNT1=/tmp/sd_p1; MNT2=/tmp/sd_p2; MNT3=/tmp/sd_p3
fi
end_step

# ---------------------------------------------------------------------------
# Populate p1 (boot FAT)
# ---------------------------------------------------------------------------
begin_step 7 "Populating p1 (boot FAT)"
run cp "$UBOOT_BIN" "$MNT1/UBOOT.BIN"
run cp "$KERNEL_BIN" "$MNT1/zImage"
run cp "$DTB_BIN" "$MNT1/ark1668_limcet_p305.dtb"
run cp "$UENV_OUT" "$MNT1/uEnv.txt"
label="UBOOT.BIN + zImage + DTB + uEnv.txt"
for f in bootlogo.raw bootlogo_usb.raw bootlogo_nand.raw bootlogo_sd.raw; do
    [[ -f "$OUTPUT_DIR/$f" ]] && { run cp "$OUTPUT_DIR/$f" "$MNT1/$f"; label="$label + $f"; }
done
[[ -f "$ARKDATA_INI" ]] && { run cp "$ARKDATA_INI" "$MNT1/arkdata.ini"; label="$label + arkdata.ini"; }
# Real, load-bearing boot-chain files, not stock leftovers (see header).
[[ -f "$STOCK_UBOOT_BIN" ]] && { run cp "$STOCK_UBOOT_BIN" "$MNT1/uboot_stock.bin"; label="$label + uboot_stock.bin"; }
[[ -f "$HYBRID_UBOOT_BIN" ]] && { run cp "$HYBRID_UBOOT_BIN" "$MNT1/uboot_hybrid.bin"; label="$label + uboot_hybrid.bin"; }
[[ -f "$REVERSINGTRACK_RAW" ]] && { run cp "$REVERSINGTRACK_RAW" "$MNT1/reversingtrack.raw"; label="$label + reversingtrack.raw"; }
[[ -f "$ZIMAGE_STOCK" ]] && { run cp "$ZIMAGE_STOCK" "$MNT1/zImage_stock"; label="$label + zImage_stock"; }
success "$label written to p1"
end_step

# ---------------------------------------------------------------------------
# Populate p2 (rootfs) — tar extract (real permissions/ownership/symlinks,
# no repair step needed) + our own overlay on top + diagnostic tools.
# ---------------------------------------------------------------------------
begin_step 8 "Populating p2 (rootfs)"
if $DRY_RUN; then
    echo -e "${DIM}  [dry-run] tar -xf $ROOTFS_TAR -C $MNT2${RESET}"
    echo -e "${DIM}  [dry-run] rsync -a $DYN_OVERLAY_DIR/ $MNT2/${RESET}"
else
    tar -xf "$ROOTFS_TAR" -C "$MNT2"
    success "rootfs.tar extracted (real symlinks/permissions/ownership/kernel modules, no repair step needed)"
    rsync -a "$DYN_OVERLAY_DIR/" "$MNT2/"
    success "firmware_overlay_dyn/ applied on top"
    # 2026-08-23: CRITICAL, hardware-confirmed real bug -- the reasoning
    # for dropping apply_rootfs_perms.sh (tar -x as real root faithfully
    # restores archive permissions now that fakeroot is fixed) only ever
    # covered the tar-extraction step above. This rsync is a SEPARATE
    # copy from a live source directory, not a tar archive -- it just
    # faithfully carries whatever mode bits are actually on disk in
    # firmware_overlay_dyn/, which reflects the git index's own recorded
    # mode. Real root cause found and fixed at the source (etc/rc.d/rcS
    # and etc/wifi_ap.sh were genuinely 644 in the git index -- not a
    # vboxsf/mount illusion this time, confirmed on this real local-disk
    # checkout with core.fileMode=true, so a normal checkout faithfully
    # reproduced the wrong bit on real disk too), but this exact bug
    # class has recurred enough times this session that a cheap,
    # explicit defensive pass here is worth keeping regardless of
    # whether the git index is currently correct.
    chmod +x "$MNT2/etc/rc.d/rcS" 2>/dev/null || true
    find "$MNT2/etc" -maxdepth 1 -name "*.sh" -exec chmod +x {} + 2>/dev/null || true
    find "$MNT2/usr/bin" "$MNT2/usr/sbin" -maxdepth 1 -type f -exec chmod +x {} + 2>/dev/null || true
fi
# Diagnostic tools (tools/*/) -- genuine, generic, confirmed reusable by
# the pipeline audit: copy every compiled binary/script/data file from
# each tools/*/ directory into /usr/bin.
TOOLS_DIR="$SCRIPT_DIR/tools"
if [[ -d "$TOOLS_DIR" ]]; then
    if $DRY_RUN; then
        echo -e "${DIM}  [dry-run] copy tools/*/<binaries/scripts/data> -> $MNT2/usr/bin/${RESET}"
    else
        mkdir -p "$MNT2/usr/bin"
        tcount=0
        for d in "$TOOLS_DIR"/*/; do
            [[ -d "$d" ]] || continue
            for f in "$d"*; do
                [[ -f "$f" ]] || continue
                base="$(basename "$f")"
                case "$base" in
                    *.py|*.c|*.h|*.inc|*.md|*.cmd|*.o|*.orig) continue ;;
                    *-debug) continue ;;
                esac
                cp -f "$f" "$MNT2/usr/bin/$base"
                chmod +x "$MNT2/usr/bin/$base" 2>/dev/null || true
                tcount=$((tcount + 1))
            done
        done
        success "Installed $tcount diagnostic tool file(s) from tools/ into /usr/bin"
    fi
fi
if ! $DRY_RUN; then
    # sshd refuses to start with over-permissive host keys.
    if [[ -f "$MNT2/etc/ssh/ssh_host_rsa_key" ]]; then
        chmod 600 "$MNT2"/etc/ssh/ssh_host_*_key
        chmod 644 "$MNT2"/etc/ssh/ssh_host_*_key.pub "$MNT2/etc/ssh/sshd_config"
    fi
fi
end_step

# ---------------------------------------------------------------------------
# p3 (userdata) — left as valid, empty ext4. custom_ui/androidauto-sidecar
# populate /data themselves on first boot; there is no stock userdata to
# carry over (this script never had that coupling to begin with).
# ---------------------------------------------------------------------------
begin_step 9 "p3 (userdata)"
info "Left empty — app populates /data on first boot"
end_step skip

# ---------------------------------------------------------------------------
# Unmount, detach, sync
# ---------------------------------------------------------------------------
begin_step 10 "Finalizing image"
if ! $DRY_RUN; then
    sync
    umount "$MNT1" "$MNT2" "$MNT3"
    rmdir "$MNT1" "$MNT2" "$MNT3"
    MNT1=""; MNT2=""; MNT3=""
    losetup -d "$LOOP"
    LOOP=""
fi
trap - EXIT
end_step

echo ""
echo -e "${GREEN}${BOLD}====== BUILD COMPLETE ======${RESET}"
echo ""
if ! $DRY_RUN; then
    sz=$(du -sh "$IMAGE" | cut -f1)
    success "$IMAGE  ($sz)"
fi
echo ""
echo -e "${BOLD}  Write to SD card or USB drive:${RESET}"
echo -e "${DIM}    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync${RESET}"
echo ""
echo -e "${BOLD}  Boot sequence:${RESET}"
echo "    Stepldr  → loads UBOOT.BIN from BOOT partition"
echo "    U-Boot   → imports uEnv.txt, shows bootlogo.raw"
echo "    U-Boot   → 1. bootusb (kernel+DTB from USB, rootfs on SD)"
echo "    U-Boot   → 2. nandboot fallback"
echo "    Kernel   → mounts p2 ext4 as the root filesystem"
echo "    rcS      → mounts p3 ext4 as /data"
echo "    (boothybrid/bootstock/bootmmc remain available as manual U-Boot-prompt"
echo "     commands for onboard-NAND recovery -- see uboot_hybrid.bin/uboot_stock.bin)"
