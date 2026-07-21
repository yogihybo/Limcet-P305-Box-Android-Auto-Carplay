#!/bin/bash
# build_tools/apply_rootfs_perms.sh — restore Unix executable bits on a rootfs tree.
#
# Why this exists: the rootfs is version-controlled and has been committed
# from Windows checkouts, where the executable bit isn't representable, so a
# freshly materialised tree can arrive with every file at mode 0644. Packing
# that as-is is silently broken: mkfs.ubifs copies each file's on-disk mode
# straight into the image (and rsync -a likewise), so busybox, sshd, and
# every /usr/bin program end up non-executable — init then can't run rcS's
# `/usr/bin/sshd &`, and in the worst case the unit won't boot at all.
#
# The git index now records the correct 100755 modes, so a normal Linux
# checkout is already fine. This script restores them defensively at pack
# time for builds run from a tree that lost them anyway (copied from Windows
# into WSL, a checkout with core.filemode off, files re-added without +x).
# It is idempotent.
#
# Policy (matches the modes recorded in git):
#   0755  everything under */bin and */sbin; *.sh; *.so and *.so.*; init
#         scripts under etc/rc.d and etc/init.d; and any other ELF program
#         or #!-script found by content anywhere in the tree.
#   0644  kernel modules (*.ko — loaded by insmod, never exec'd) and data.
set -e

ROOT="${1:?usage: build_tools/apply_rootfs_perms.sh <rootfs-dir>}"
[ -d "$ROOT" ] || { echo "apply_rootfs_perms: not a directory: $ROOT" >&2; exit 1; }

# 1. Convention: paths and names that are always executable.
find "$ROOT" -type f \( \
       -path '*/bin/*'  -o -path '*/sbin/*' \
    -o -path '*/etc/rc.d/*' -o -path '*/etc/init.d/*' \
    -o -name '*.sh' -o -name '*.so' -o -name '*.so.*' \
  \) -exec chmod 0755 {} +

# 2. Content sniff for the stragglers pass 1 can't name — ELF programs and
#    #!-scripts that live outside the dirs above (e.g. usr/lib/MFITest,
#    etc/udhcpc.script, usr/ssl/misc/*.pl). Only files that aren't already
#    executable are read, and kernel modules are skipped so they stay 0644.
find "$ROOT" -type f ! -name '*.ko' ! -perm -u+x -print0 |
while IFS= read -r -d '' f; do
    sig=$(od -An -N4 -tx1 "$f" 2>/dev/null | tr -d ' \n')
    case "$sig" in
        7f454c46*) chmod 0755 "$f" ;;   # ELF   (\x7fELF)
        2321*)     chmod 0755 "$f" ;;   # "#!"  shebang
    esac
done

echo "    Permission exec bits restored on $(basename "$ROOT") ($(find "$ROOT" -type f -perm -u+x | wc -l) executable files)"
