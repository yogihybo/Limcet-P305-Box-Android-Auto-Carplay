#!/bin/bash
# Thin wrapper around build_bootable_sdcard.sh (repo root) for the new,
# general-purpose, dynamically-linked rootfs (Buildroot's
# ark1668_ft_dyn_defconfig + firmware_overlay_dyn/), Phase 5 of
# merry-snacking-wirth.md. custom_ui/androidauto-sidecar are its
# current primary workload, not its defining scope.
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
# firmware_overlay_dyn/ on top a second time -- last write wins,
# so the final image ends up with this rootfs's own rcS/inittab/
# profile/BlueZ/WiFi wiring, not stock's.
#
# Usage:
#   ./build_bootable_sdcard_dyn.sh [options] [-- <extra args passed to build_bootable_sdcard.sh>]
#
# Options:
#   --arkmicro-dir DIR          linux-arkmicro repo root (default:
#                                autodetected: sibling dir, then
#                                /home/osboxes/Downloads/linux-arkmicro, then
#                                ~/Downloads/linux-arkmicro of the REAL
#                                invoking user even under sudo -- explicit
#                                override in case none of those are right)
#   --buildroot-output-dir DIR  Buildroot output/ tree directly (default:
#                                $ARKMICRO_DIR/buildroot/output; overrides
#                                --arkmicro-dir's derived value if both given)
#   --dyn-overlay DIR           firmware_overlay_dyn/ (default: next to
#                                this script)
#   --merged-dir DIR            scratch dir for the pre-merged rootfs (default:
#                                a fresh dir under this script's own build/)
#   --image PATH                passed through to build_bootable_sdcard.sh
#   --skip-build                Skip the automatic `make ui androidauto-sidecar`
#                                + re-stage step below and deploy whatever's
#                                currently sitting in firmware_overlay_dyn/ as-is
#                                (only for when you've already built/staged by
#                                hand and want to skip the rebuild -- the
#                                default behavior exists specifically so this
#                                is never required for a normal deploy).
#   --dry-run                   passed through
#   --non-interactive           passed through (recommended for scripted use)
#   --help                      show this help
#
# 2026-08-22: this wrapper now ALWAYS rebuilds custom_ui/androidauto-sidecar
# and re-stages the fresh binaries + configs into firmware_overlay_dyn/usr/bin/
# before deploying, unless --skip-build is passed -- a real staleness bug hit
# this session (the overlay was caught, by chance, holding pre-migration
# binaries from before a toolchain swap) showed the old manual "make, then
# remember to cp" workflow had no safety net at all. Fails loudly (set -e)
# if the build fails; never proceeds to image creation with a stale binary.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 2026-08-22: real bug hit on an actual `sudo ./build_bootable_sdcard_dyn.sh`
# deploy -- under sudo, $HOME is /root (root's home), not the invoking
# user's real home, so the old unconditional "$HOME/Downloads/linux-arkmicro"
# default silently pointed at a nonexistent /root/Downloads/... and
# reported the real build output "not found" even though it existed at
# the real user's own $HOME. build_bootable_sdcard.sh's own autodetect()
# never actually has this problem in practice: it tries a sibling-repo
# candidate ($SCRIPT_DIR/../linux-arkmicro) and a hardcoded absolute
# fallback specific to this dev machine BEFORE ever falling back to
# $HOME -- this wrapper had copied only the naive $HOME-only pattern
# without that fallback chain. Fixed to match (and improve on) the
# original: resolve the real invoking user's home via $SUDO_USER when
# running under sudo, and try the same sibling-repo-first chain.
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

BUILDROOT_OUTPUT_DIR="${BUILDROOT_OUTPUT_DIR:-}"
DYN_OVERLAY_DIR="$SCRIPT_DIR/firmware_overlay_dyn"
MERGED_DIR="$SCRIPT_DIR/build/dyn_rootfs_merged"
IMAGE=""
PASSTHROUGH_ARGS=()
DRY_RUN=false
SKIP_BUILD=false

usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arkmicro-dir)         ARKMICRO_DIR="$2"; shift 2 ;;
        --buildroot-output-dir) BUILDROOT_OUTPUT_DIR="$2"; shift 2 ;;
        --dyn-overlay)          DYN_OVERLAY_DIR="$2"; shift 2 ;;
        --merged-dir)           MERGED_DIR="$2"; shift 2 ;;
        --image)                IMAGE="$2"; PASSTHROUGH_ARGS+=(--image "$2"); shift 2 ;;
        --skip-build)            SKIP_BUILD=true; shift ;;
        --dry-run)               DRY_RUN=true; PASSTHROUGH_ARGS+=(--dry-run); shift ;;
        --help|-h)               usage ;;
        --)                      shift; PASSTHROUGH_ARGS+=("$@"); break ;;
        *)                       PASSTHROUGH_ARGS+=("$1"); shift ;;
    esac
done

# --buildroot-output-dir (or the env var) always wins outright; otherwise
# derive it from ARKMICRO_DIR (autodetected above, or overridden via
# --arkmicro-dir) -- computed here, after flag parsing, so
# --arkmicro-dir actually takes effect regardless of flag order.
[[ -z "$BUILDROOT_OUTPUT_DIR" ]] && BUILDROOT_OUTPUT_DIR="$ARKMICRO_DIR/buildroot/output"

# 2026-08-23: checks output/images/rootfs.tar now, not output/target/
# directly -- the pre-merge step below extracts Buildroot's own
# official tar export (BR2_TARGET_ROOTFS_TAR) instead of rsyncing the
# raw target dir, see that step's own comment for why.
[[ -d "$BUILDROOT_OUTPUT_DIR/target" ]] || {
    echo "ERROR: Buildroot target dir not found: $BUILDROOT_OUTPUT_DIR/target" >&2
    echo "       Build ark1668_ft_dyn_defconfig first (see merry-snacking-wirth.md)." >&2
    exit 1
}
[[ -f "$BUILDROOT_OUTPUT_DIR/images/rootfs.tar" ]] || {
    echo "ERROR: rootfs.tar not found: $BUILDROOT_OUTPUT_DIR/images/rootfs.tar" >&2
    echo "       Enable BR2_TARGET_ROOTFS_TAR and rebuild ark1668_ft_dyn_defconfig first." >&2
    exit 1
}
[[ -d "$DYN_OVERLAY_DIR" ]] || {
    echo "ERROR: firmware_overlay_dyn/ not found: $DYN_OVERLAY_DIR" >&2
    exit 1
}
[[ -x "$SCRIPT_DIR/build_bootable_sdcard.sh" ]] || {
    echo "ERROR: build_bootable_sdcard.sh not found next to this script." >&2
    exit 1
}

# Default image path, same convention as the original script, but a
# distinct filename so the two never collide/overwrite each other.
[[ -z "$IMAGE" ]] && {
    IMAGE="$SCRIPT_DIR/sd_bootable/sd_boot_dyn.img"
    PASSTHROUGH_ARGS+=(--image "$IMAGE")
}

CUSTOM_UI_DIR="$SCRIPT_DIR/custom_ui"
if $SKIP_BUILD; then
    echo "==> --skip-build passed -- deploying whatever's currently staged in $DYN_OVERLAY_DIR/usr/bin/ as-is"
elif $DRY_RUN; then
    echo "  [dry-run] make -C $CUSTOM_UI_DIR ui androidauto-sidecar"
    echo "  [dry-run] cp $CUSTOM_UI_DIR/build/{custom_ui,androidauto-sidecar,hal.conf,default_settings.conf} $DYN_OVERLAY_DIR/usr/bin/"
    echo "  [dry-run] rsync -a $CUSTOM_UI_DIR/build/alsa/ $DYN_OVERLAY_DIR/usr/bin/alsa/"
else
    echo "==> Building custom_ui/androidauto-sidecar fresh (BUILDROOT_OUTPUT_DIR=$BUILDROOT_OUTPUT_DIR, HOME=$REAL_HOME)"
    # set -e means a failed build stops this script here, before any image
    # work starts -- this is the whole point: never let a stale overlay
    # binary reach $IMAGE silently. Real bug this closes: the overlay was
    # found holding a pre-toolchain-migration custom_ui/androidauto-sidecar
    # (an old manual `make`+`cp` was never redone after the glibc 2.30
    # swap) during this session's overlay re-verification pass -- caught by
    # chance via md5sum, not by anything in this pipeline.
    #
    # HOME="$REAL_HOME" (not the ambient $HOME, /root under sudo): this
    # Makefile has several of its own $(HOME)/build-deps-style defaults
    # (AASDK_DEPS_DIR, AASDK_DEPS_DIR_DYN, its own BUILDROOT_OUTPUT_DIR
    # fallback), and every custom_ui/third_party/build_*.sh script make
    # can invoke as a sub-build has the exact same $HOME-default pattern
    # -- all of them inherit HOME from this process's environment as a
    # plain child process, so overriding it once here fixes every one of
    # them transitively, without hunting down and rewriting each
    # reference individually (which is exactly how the previous
    # BUILDROOT_OUTPUT_DIR-only fix missed this dbus/dbus.h failure: the
    # env var override on the make command line only covers vars the
    # Makefile actually reads that way, not the general "$HOME under
    # sudo is wrong" problem underneath all of them). Explicit
    # AASDK_DEPS_DIR/AASDK_DEPS_DIR_DYN passthrough was also tried as a
    # narrower alternative during a real deploy attempt -- redundant
    # once HOME itself is corrected (both vars already default to
    # $HOME/build-deps*, which now resolves correctly), so not kept
    # here to avoid two sources of truth for the same path.
    HOME="$REAL_HOME" BUILDROOT_OUTPUT_DIR="$BUILDROOT_OUTPUT_DIR" make -C "$CUSTOM_UI_DIR" ui androidauto-sidecar
    echo "==> Re-staging fresh binaries + configs into $DYN_OVERLAY_DIR/usr/bin/"
    cp -f "$CUSTOM_UI_DIR/build/custom_ui" "$CUSTOM_UI_DIR/build/androidauto-sidecar" \
          "$CUSTOM_UI_DIR/build/hal.conf" "$CUSTOM_UI_DIR/build/default_settings.conf" \
          "$DYN_OVERLAY_DIR/usr/bin/"
    rsync -a "$CUSTOM_UI_DIR/build/alsa/" "$DYN_OVERLAY_DIR/usr/bin/alsa/"
    chmod +x "$DYN_OVERLAY_DIR/usr/bin/custom_ui" "$DYN_OVERLAY_DIR/usr/bin/androidauto-sidecar"
fi

# 2026-08-23: switched from rsyncing output/target/ directly to
# extracting Buildroot's own official rootfs.tar (BR2_TARGET_ROOTFS_TAR,
# enabled alongside the existing ext4 target in
# ark1668_ft_dyn_defconfig). Real reasons, not just style: the tar is
# built via Buildroot's own inner-rootfs macro (fs/common.mk), which
# (a) excludes THIS_IS_NOT_YOUR_ROOT_FILESYSTEM by construction (no
# manual --exclude needed any more -- kept below anyway, see its own
# comment) and (b) runs target/ through a real fakeroot pass (chown,
# mkusers, makedevs -d against the actual permission table) before
# archiving, which a raw rsync of output/target/ never got at all.
#
# Real caveat, found and verified on THIS dev machine, not assumed:
# fakeroot's chown-faking is broken in this specific sandbox (isolated
# test: `chown -h -R 0:0` inside a fakeroot session, then `stat`
# immediately after in the SAME session, still reports the build
# user's real uid/gid, not 0:0 -- confirmed via two independent tests,
# with and without FAKEROOTDONTTRYCHOWN). chmod-based faking (setuid
# bits etc.) DOES work correctly, confirmed the same way. So the
# ownership-normalization benefit does not actually materialize here
# -- rootfs.tar still carries the build user's uid/gid, same as the
# old rsync path did -- but the placeholder-file exclusion and the
# architectural benefit (one less manually-patched leak class, using
# Buildroot's own tested export path) are both real regardless, and
# ownership would likely self-correct on an unrestricted machine
# without this sandbox's fakeroot limitation.
#
# Extracting requires real root (device nodes need CAP_MKNOD, chown
# during extraction needs root) -- this whole script is already
# expected to run under sudo (see REAL_HOME resolution above), so this
# is not a new privilege requirement.
ROOTFS_TAR="$BUILDROOT_OUTPUT_DIR/images/rootfs.tar"
echo "==> Pre-merging Buildroot rootfs.tar + firmware_overlay_dyn into $MERGED_DIR"
rm -rf "$MERGED_DIR"
mkdir -p "$MERGED_DIR"
if $DRY_RUN; then
    echo "  [dry-run] tar -xf $ROOTFS_TAR -C $MERGED_DIR"
    echo "  [dry-run] rsync -a $DYN_OVERLAY_DIR/ $MERGED_DIR/"
else
    [[ -f "$ROOTFS_TAR" ]] || {
        echo "ERROR: rootfs.tar not found: $ROOTFS_TAR" >&2
        echo "       Enable BR2_TARGET_ROOTFS_TAR and rebuild ark1668_ft_dyn_defconfig first." >&2
        exit 1
    }
    tar -xf "$ROOTFS_TAR" -C "$MERGED_DIR"
    # Our overlay wins over Buildroot's raw target output here (e.g.
    # rcS/inittab/profile all get replaced by the trimmed Phase 3
    # versions) -- same ordering convention as build_bootable_sdcard.sh's
    # own apply_overlay() applying firmware_overlay/ after the main
    # rootfs sync for the stock path.
    rsync -a "$DYN_OVERLAY_DIR/" "$MERGED_DIR/"
fi

echo "==> Running build_bootable_sdcard.sh --rootfs-dir $MERGED_DIR"
"$SCRIPT_DIR/build_bootable_sdcard.sh" --rootfs-dir "$MERGED_DIR" "${PASSTHROUGH_ARGS[@]}"

if $DRY_RUN; then
    echo "[dry-run] Would now re-apply firmware_overlay_dyn/ onto $IMAGE's p2 a second time"
    echo "[dry-run] (undoes build_bootable_sdcard.sh's own apply_overlay(), which unconditionally"
    echo "[dry-run]  rsyncs the STOCK firmware_overlay/ on top afterward -- see this script's own"
    echo "[dry-run]  header comment for why that second pass is necessary)"
    exit 0
fi

[[ -f "$IMAGE" ]] || {
    echo "ERROR: expected output image not found at $IMAGE after build_bootable_sdcard.sh ran." >&2
    exit 1
}

echo "==> Re-applying firmware_overlay_dyn/ onto $IMAGE (last write wins over stock's own apply_overlay())"
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
rsync -a "$DYN_OVERLAY_DIR/" "$MNT/"
# 2026-08-22: found a stale stock /README.md actually shipping on a
# real built image -- build_bootable_sdcard.sh's own apply_overlay()
# (never modified here) rsyncs the ENTIRE stock firmware_overlay/ tree
# onto p2, including firmware_overlay/README.md itself (documentation
# for that source directory, never meant to be a deployed file) ->
# lands at real /README.md. firmware_overlay_dyn/ has no top-level
# README.md of its own to overwrite it with on this second pass, so it
# survived untouched -- 20KB of stock MsnCoreApp/blueware/sink
# documentation with no relevance to this rootfs. Not something this
# session's overlay work ever intended to ship; remove it rather than
# replace it -- this rootfs's own documentation belongs in this
# source tree (README.md/docs/), not baked into the deployed image.
rm -f "$MNT/README.md"
# Same leak, same cause, same fix: busybox-applets.manifest is a
# build-time-only input (build_bootable_sdcard.sh's own
# apply_busybox_applets() reads it from $OVERLAY_DIR to generate
# symlinks during the build) that rides along in
# firmware_overlay/'s own top level and gets rsynced onto the real
# image the same way README.md did -- not a runtime file, nothing on
# the device ever reads it after boot.
rm -f "$MNT/busybox-applets.manifest"
# Permission safety net: this second-pass rsync is the ONLY thing that
# lands firmware_overlay_dyn/'s own rcS/wifi_ap.sh and every usr/bin//
# usr/sbin binary (custom_ui, androidauto-sidecar, sshd, bluetoothd,
# rtk_hciattach, hci-updown) on the real image -- it runs AFTER (and
# overwrites) build_bootable_sdcard.sh's own main-rootfs-sync step,
# which already runs build_tools/apply_rootfs_perms.sh (real,
# comprehensive: path convention for */bin//*/sbin/etc.rc.d/*.sh/*.so
# PLUS ELF-magic/shebang content-sniffing for stragglers) on
# /tmp/sd_p2 right after syncing $ROOTFS_DIR (this wrapper's own
# pre-merged $MERGED_DIR, which already includes firmware_overlay_dyn/
# from the FIRST pass above) -- so the first pass's permissions are
# already covered. This second pass is a distinct, later rsync
# straight onto the finished image's mounted p2, entirely outside that
# mechanism, so reuse the same real script here rather than a
# hand-rolled subset -- same reasoning as build_bootable_sdcard.sh's
# own header comment: this vboxsf-mounted checkout doesn't reliably
# carry real Unix permission bits through rsync -a, so don't trust the
# source tree's mode bits even though the git index itself is now
# correct (fixed 2026-08-22).
bash "$SCRIPT_DIR/build_tools/apply_rootfs_perms.sh" "$MNT"
# sshd host key permissions: same vboxsf-carried-777 problem as above,
# but these need to be LESS permissive, not executable -- sshd refuses
# to start with over-permissive host keys ("Permissions ... are too
# open").
if [[ -f "$MNT/etc/ssh/ssh_host_rsa_key" ]]; then
    chmod 600 "$MNT"/etc/ssh/ssh_host_*_key
    chmod 644 "$MNT"/etc/ssh/ssh_host_*_key.pub "$MNT/etc/ssh/sshd_config"
fi
sync
umount "$MNT"
rmdir "$MNT"
MNT=""
losetup -d "$LOOP"
LOOP=""
trap - EXIT

echo "==> Done: $IMAGE"
echo "    sudo dd if=\"$IMAGE\" of=/dev/sdX bs=4M status=progress && sync"
