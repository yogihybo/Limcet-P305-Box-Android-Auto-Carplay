#!/bin/bash
# build_tools/restore_rootfs_symlinks.sh — recreate the rootfs symlinks that a Windows
# checkout / extraction drops.
#
# Why this exists: the rootfs is version-controlled and is often materialised
# by extracting a vendor `rootfs.img` (UBIFS) on Windows. Windows/`git
# core.symlinks=false` cannot represent POSIX symlinks, so every symlink is
# silently lost — the tree ends up with `/bin/busybox` but no `/bin/sh`,
# `/bin/ls`, `/bin/mount`, no `/sbin/init`, no `/lib/libc.so.6`, etc. Packing
# that with mkfs.ubifs (or copying it with rsync -a) produces an image that
# cannot boot: `inittab` runs `-/bin/sh` and `rcS` runs `mount`, both of which
# are just missing.
#
# mkfs.ubifs does NOT invent symlinks — it copies whatever is on disk. So they
# must be recreated before packing. This script does that from a manifest.
#
# The manifest (`build_tools/rootfs.symlinks`) is `linkpath<TAB>target`, one per line, paths
# relative to the rootfs root, exactly as stored in the image. It was harvested
# from the complete `Holden firmware update/rootfs.img` (the P306/C235 update
# images carry the byte-identical set), which is the authoritative source: an
# image extracted on Linux preserves these symlinks natively; this script just
# reproduces them on a host that couldn't.
#
# It is idempotent, and it WARNS (does not fail) on a symlink whose target is
# absent from the tree — a dangling target means the tree is missing a real
# file, which is worth surfacing rather than hiding.
#
# Usage: build_tools/restore_rootfs_symlinks.sh <rootfs-dir> [manifest]
set -e

ROOT="${1:?usage: build_tools/restore_rootfs_symlinks.sh <rootfs-dir> [manifest]}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="${2:-$SCRIPT_DIR/rootfs.symlinks}"

[ -d "$ROOT" ]     || { echo "restore_rootfs_symlinks: not a directory: $ROOT" >&2; exit 1; }
[ -f "$MANIFEST" ] || { echo "restore_rootfs_symlinks: manifest not found: $MANIFEST" >&2; exit 1; }

created=0 already=0 dangling=0

# Pass 1 — create every symlink. Read the manifest tolerant of a stray CR
# (Windows) on the target field.
while IFS=$'\t' read -r link target || [ -n "$link" ]; do
    [ -z "$link" ] && continue
    case "$link" in \#*) continue ;; esac
    target="${target%$'\r'}"

    linkpath="$ROOT/$link"
    mkdir -p "$(dirname "$linkpath")"

    if [ -L "$linkpath" ] && [ "$(readlink "$linkpath")" = "$target" ]; then
        already=$((already + 1))
    else
        # Replace whatever is there (a lost-symlink placeholder file, wrong
        # link, or nothing) with the correct symlink.
        rm -f "$linkpath"
        ln -s "$target" "$linkpath"
        created=$((created + 1))
    fi
done < "$MANIFEST"

# Pass 2 — validate. Done only after every link exists so that symlink CHAINS
# resolve (e.g. libpng.so -> libpng15.so -> libpng15.so.15.30.0). `test -e`
# follows the whole chain, so a link only counts as dangling when the final
# real file is genuinely absent from the tree.
while IFS=$'\t' read -r link target || [ -n "$link" ]; do
    [ -z "$link" ] && continue
    case "$link" in \#*) continue ;; esac
    if [ ! -e "$ROOT/$link" ]; then
        dangling=$((dangling + 1))
        [ "$dangling" -le 10 ] && echo "  WARN dangling: $link -> ${target%$'\r'}" >&2
    fi
done < "$MANIFEST"

echo "    Symlinks restored on $(basename "$ROOT"): $created created/fixed, $already already correct, $dangling dangling"
if [ "$dangling" -gt 0 ]; then
    echo "  ($dangling symlink target(s) missing from the tree — the source tree is" >&2
    echo "   incomplete, not just missing symlinks. See the extraction source.)" >&2
fi
