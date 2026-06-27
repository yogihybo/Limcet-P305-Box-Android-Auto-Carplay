#!/bin/bash
# Rebuild flashable userdata UBI image for ARK1680 (Prado / Limcet-P306)
#
# Requires: mkfs.ubifs, ubinize  (apt install mtd-utils on Debian/Ubuntu)
# Run under Linux or WSL.
#
# ARK1680 NAND geometry (from live device printenv):
#   nand_writesize = 0x800   = 2048 bytes  (page size)
#   nand_erasesize = 0x20000 = 131072 bytes (PEB size)
#
# UBI / userdata partition parameters:
#   LEB size     = PEB - 2 * page = 131072 - 4096 = 126976 bytes
#   userdata partition = 6m = 6,291,456 bytes
#   max LEBs     = ceil(6291456 / 126976) = 50
#
# Userdata layout:
#   msndatadef            — empty marker file (Holden base)
#   msncfg/               — Prado-specific settings overlay
#     MsnProductInfo.ini  — hardware identity (copied from msn_factory_configs/)
#     FactoryConfig.ini   — device branding and link settings
#     Setting.config      — screen brightness, volume defaults
#     carsetting.ini      — car-specific enables
#     AndroidMirrorLink.ini
#     StartupApp.config
#
# On first boot the application will also create:
#   feasycom/             — BT module config (written by blueware)
#   msncfg/btcfg/         — BT call history (empty on fresh flash)
#   pointercal            — touchscreen calibration (written by tslib)
#   udhcpd.leases         — DHCP leases (written at runtime)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USERDATA_SRC="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata"
BUILD_DIR="$(mktemp -d)"
UBIFS_IMAGE="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata.ubifs"
UBI_IMAGE="$SCRIPT_DIR/Prado reconstructed/mtd7_userdata/userdata.img"
UBI_CFG="$BUILD_DIR/ubi.cfg"

MIN_IO=2048
LEB_SIZE=126976
MAX_LEBS=50

# Copy userdata source tree into build dir, then overlay msn_factory_configs
echo "=== Preparing userdata tree ==="
cp -r "$USERDATA_SRC/." "$BUILD_DIR/fs/"

# Overlay: copy Prado MsnProductInfo and FactoryConfig into msncfg
mkdir -p "$BUILD_DIR/fs/msncfg"
cp "$SCRIPT_DIR/msn_factory_configs/MsnProductInfo.ini" "$BUILD_DIR/fs/msncfg/"
cp "$SCRIPT_DIR/msn_factory_configs/FactoryConfig.ini"  "$BUILD_DIR/fs/msncfg/"

echo "  userdata tree:"
find "$BUILD_DIR/fs" -type f | sed "s|$BUILD_DIR/fs/||" | sort | sed 's/^/    /'

echo ""
echo "=== Building UBIFS image ==="
mkfs.ubifs \
    -r "$BUILD_DIR/fs" \
    -m $MIN_IO \
    -e $LEB_SIZE \
    -c $MAX_LEBS \
    -o "$UBIFS_IMAGE" \
    -v

echo ""
echo "=== Building UBI image ==="
cat > "$UBI_CFG" << 'EOF'
[userdata]
mode=ubi
image=Prado reconstructed/mtd7_userdata/userdata.ubifs
vol_id=0
vol_type=dynamic
vol_name=userdata
vol_flags=autoresize
EOF

# ubinize paths are relative to the script dir
cd "$SCRIPT_DIR"
ubinize \
    -o "$UBI_IMAGE" \
    -m $MIN_IO \
    -p 131072 \
    -s 512 \
    "$UBI_CFG" \
    -v

rm -rf "$BUILD_DIR"

echo ""
echo "=== Done ==="
echo "Flashable image: $UBI_IMAGE"
echo "Size: $(du -h "$UBI_IMAGE" | cut -f1)"
echo ""
echo "Flash commands (Prado — 6m userdata at 0x6FA0000):"
echo "  nand scrub 0x6fa0000 0x600000 0x6fa0000 0x600000"
echo "  nand write 0x4000000 0x6fa0000 \$(filesize)"
echo ""
echo "WARNING: This erases all paired devices, call history, and user settings."
echo "         The application will recreate defaults on first boot."
