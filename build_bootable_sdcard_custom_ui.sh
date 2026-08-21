#!/bin/bash
# Thin wrapper around build_bootable_sdcard.sh (repo root) for the new
# dynamically-linked custom_ui rootfs (Buildroot's ark1668_ft_custom_ui_defconfig
# + firmware_overlay_custom_ui/), Phase 5 of merry-snacking-wirth.md.
#
# build_bootable_sdcard.sh itself is NEVER modified -- reused via its
# existing --rootfs-dir flag. But --rootfs-dir alone is NOT enough:
# that script's own apply_overlay() unconditionally rsyncs the STOCK
# firmware_overlay/ onto the SD image AFTER the main rootfs sync,
# whenever the new-kernel path is used (the default, and what this
# rootfs needs too -- same shared 4.19.192 kernel). OVERLAY_DIR is a
# plain hardcoded script variable, not a CLI flag and not
# environment-overridable (`OVERLAY_DIR="$SCRIPT_DIR/firmware_overlay"`,
# not `:=`), so there is no way to redirect or skip that step from
# outside the script -- confirmed by reading its actual source, not
# assumed from the plan's earlier (incomplete) description of this
# seam.
#
# Real fix, still without touching the original script: let it run
# (stock's overlay lands on top, unavoidable), then THIS wrapper
# re-attaches the same output image afterward and re-applies
# firmware_overlay_custom_ui/ on top a second time -- last write wins,
# so the final image ends up with this rootfs's own rcS/inittab/
# profile/BlueZ/WiFi wiring, not stock's.
#
# Usage:
#   ./build_bootable_sdcard_custom_ui.sh [options] [-- <extra args passed to build_bootable_sdcard.sh>]
#
# Options:
#   --buildroot-output-dir DIR  Buildroot output/ tree (default:
#                                ~/Downloads/linux-arkmicro/buildroot/output)
#   --custom-ui-overlay DIR     firmware_overlay_custom_ui/ (default: next to
#                                this script)
#   --merged-dir DIR            scratch dir for the pre-merged rootfs (default:
#                                a fresh dir under this script's own build/)
#   --image PATH                passed through to build_bootable_sdcard.sh
#   --dry-run                   passed through
#   --non-interactive           passed through (recommended for scripted use)
#   --help                      show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILDROOT_OUTPUT_DIR="${BUILDROOT_OUTPUT_DIR:-$HOME/Downloads/linux-arkmicro/buildroot/output}"
CUSTOM_UI_OVERLAY="$SCRIPT_DIR/firmware_overlay_custom_ui"
MERGED_DIR="$SCRIPT_DIR/build/custom_ui_rootfs_merged"
IMAGE=""
PASSTHROUGH_ARGS=()
DRY_RUN=false

usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --buildroot-output-dir) BUILDROOT_OUTPUT_DIR="$2"; shift 2 ;;
        --custom-ui-overlay)    CUSTOM_UI_OVERLAY="$2"; shift 2 ;;
        --merged-dir)           MERGED_DIR="$2"; shift 2 ;;
        --image)                IMAGE="$2"; PASSTHROUGH_ARGS+=(--image "$2"); shift 2 ;;
        --dry-run)               DRY_RUN=true; PASSTHROUGH_ARGS+=(--dry-run); shift ;;
        --help|-h)               usage ;;
        --)                      shift; PASSTHROUGH_ARGS+=("$@"); break ;;
        *)                       PASSTHROUGH_ARGS+=("$1"); shift ;;
    esac
done

TARGET_DIR="$BUILDROOT_OUTPUT_DIR/target"
[[ -d "$TARGET_DIR" ]] || {
    echo "ERROR: Buildroot target dir not found: $TARGET_DIR" >&2
    echo "       Build ark1668_ft_custom_ui_defconfig first (see merry-snacking-wirth.md)." >&2
    exit 1
}
[[ -d "$CUSTOM_UI_OVERLAY" ]] || {
    echo "ERROR: firmware_overlay_custom_ui/ not found: $CUSTOM_UI_OVERLAY" >&2
    exit 1
}
[[ -x "$SCRIPT_DIR/build_bootable_sdcard.sh" ]] || {
    echo "ERROR: build_bootable_sdcard.sh not found next to this script." >&2
    exit 1
}

# Default image path, same convention as the original script, but a
# distinct filename so the two never collide/overwrite each other.
[[ -z "$IMAGE" ]] && {
    IMAGE="$SCRIPT_DIR/sd_bootable/sd_boot_custom_ui.img"
    PASSTHROUGH_ARGS+=(--image "$IMAGE")
}

echo "==> Pre-merging Buildroot output/target + firmware_overlay_custom_ui into $MERGED_DIR"
rm -rf "$MERGED_DIR"
mkdir -p "$MERGED_DIR"
if $DRY_RUN; then
    echo "  [dry-run] rsync -a $TARGET_DIR/ $MERGED_DIR/"
    echo "  [dry-run] rsync -a $CUSTOM_UI_OVERLAY/ $MERGED_DIR/"
else
    rsync -a "$TARGET_DIR/" "$MERGED_DIR/"
    # Our overlay wins over Buildroot's raw target output here (e.g.
    # rcS/inittab/profile all get replaced by the trimmed Phase 3
    # versions) -- same ordering convention as build_bootable_sdcard.sh's
    # own apply_overlay() applying firmware_overlay/ after the main
    # rootfs sync for the stock path.
    rsync -a "$CUSTOM_UI_OVERLAY/" "$MERGED_DIR/"
fi

echo "==> Running build_bootable_sdcard.sh --rootfs-dir $MERGED_DIR"
"$SCRIPT_DIR/build_bootable_sdcard.sh" --rootfs-dir "$MERGED_DIR" "${PASSTHROUGH_ARGS[@]}"

if $DRY_RUN; then
    echo "[dry-run] Would now re-apply firmware_overlay_custom_ui/ onto $IMAGE's p2 a second time"
    echo "[dry-run] (undoes build_bootable_sdcard.sh's own apply_overlay(), which unconditionally"
    echo "[dry-run]  rsyncs the STOCK firmware_overlay/ on top afterward -- see this script's own"
    echo "[dry-run]  header comment for why that second pass is necessary)"
    exit 0
fi

[[ -f "$IMAGE" ]] || {
    echo "ERROR: expected output image not found at $IMAGE after build_bootable_sdcard.sh ran." >&2
    exit 1
}

echo "==> Re-applying firmware_overlay_custom_ui/ onto $IMAGE (last write wins over stock's own apply_overlay())"
LOOP=""
MNT=""
cleanup() {
    [[ -n "$MNT" && -d "$MNT" ]] && { mountpoint -q "$MNT" && umount "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null; }
    [[ -n "$LOOP" ]] && losetup -d "$LOOP" 2>/dev/null
}
trap cleanup EXIT

LOOP=$(losetup -Pf --show "$IMAGE")
MNT=$(mktemp -d)
mount "${LOOP}p2" "$MNT"
rsync -a "$CUSTOM_UI_OVERLAY/" "$MNT/"
sync
umount "$MNT"
rmdir "$MNT"
MNT=""
losetup -d "$LOOP"
LOOP=""
trap - EXIT

echo "==> Done: $IMAGE"
echo "    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync"
