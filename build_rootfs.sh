#!/bin/bash
# Rebuild flashable rootfs UBI image for ARK1680 (Prado / Limcet-P306)
#
# Requires: mkfs.ubifs, ubinize  (apt install mtd-utils on Debian/Ubuntu)
# Run under Linux or WSL.
#
# ARK1680 NAND geometry (from live device printenv):
#   nand_writesize = 0x800  = 2048 bytes  (page size)
#   nand_erasesize = 0x20000 = 131072 bytes (PEB size)
#   nand_oobsize   = 0x40  = 64 bytes
#
# UBI parameters derived from geometry:
#   LEB size = PEB - 2 * page = 131072 - 4096 = 126976 bytes
#   rootfs partition = 106m = 111,149,056 bytes
#   max LEBs = ceil(111149056 / 126976) = 875

set -e

# mtd-utils installs to /usr/sbin, which isn't always on $PATH (e.g. WSL,
# non-login shells) even when the package is installed.
export PATH="$PATH:/usr/sbin:/sbin"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOTFS_DIR="$SCRIPT_DIR/firmware_source/prado_reconstructed/mtd6_rootfs/rootfs"
UBIFS_IMAGE="$SCRIPT_DIR/firmware_source/prado_reconstructed/mtd6_rootfs/rootfs.ubifs"
UBI_IMAGE="$SCRIPT_DIR/firmware_source/prado_reconstructed/mtd6_rootfs/rootfs.img"
UBI_CFG="$SCRIPT_DIR/firmware_source/prado_reconstructed/mtd6_rootfs/ubi.cfg"

MIN_IO=2048
LEB_SIZE=126976
MAX_LEBS=875

echo "=== Restoring rootfs symlinks ==="
bash "$SCRIPT_DIR/build_tools/restore_rootfs_symlinks.sh" "$ROOTFS_DIR"

echo "=== Restoring rootfs exec bits ==="
bash "$SCRIPT_DIR/build_tools/apply_rootfs_perms.sh" "$ROOTFS_DIR"

echo "=== Building UBIFS image ==="
mkfs.ubifs \
    -r "$ROOTFS_DIR" \
    -m $MIN_IO \
    -e $LEB_SIZE \
    -c $MAX_LEBS \
    -o "$UBIFS_IMAGE" \
    -v

echo ""
echo "=== Writing ubinize config ==="
cat > "$UBI_CFG" << 'EOF'
[rootfs]
mode=ubi
image=firmware_source/prado_reconstructed/mtd6_rootfs/rootfs.ubifs
vol_id=0
vol_type=dynamic
vol_name=rootfs
vol_flags=autoresize
EOF

echo "=== Building UBI image ==="
ubinize \
    -o "$UBI_IMAGE" \
    -m $MIN_IO \
    -p 131072 \
    -s 512 \
    "$UBI_CFG" \
    -v

echo ""
echo "=== Done ==="
echo "Flashable image: $UBI_IMAGE"
echo "Size: $(du -h "$UBI_IMAGE" | cut -f1)"
echo ""
echo "Flash command (Prado — 106m rootfs at 0x5A0000):"
echo "  nand scrub 0x5a0000 0x6a00000 0x5a0000 0x6a00000"
echo "  nand write 0x4000000 0x5a0000 \$(filesize)"
