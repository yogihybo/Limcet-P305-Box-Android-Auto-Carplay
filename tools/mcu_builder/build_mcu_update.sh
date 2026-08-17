#!/usr/bin/env bash
# ==============================================================================
# Build & Package Script: MCU Firmware Update for Limcet Box (STM32F105)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

STOCK_BIN="${REPO_ROOT}/hardware/MCU/can_app.bin"
OUTPUT_DIR="${SCRIPT_DIR}/output"
PACKAGE_DIR="${OUTPUT_DIR}/usb_root"
ZIP_OUTPUT="${OUTPUT_DIR}/mcu_update_package.zip"

CONFIG_FILE="${SCRIPT_DIR}/can_config_template.json"
PRESET=""

print_usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -c, --config FILE       Path to custom CAN JSON config file
  -p, --preset PRESET     Vehicle preset (e.g. toyota_prado_150)
  -i, --input BINARY      Input stock can_app.bin (default: hardware/MCU/can_app.bin)
  -o, --output-zip ZIP    Output zip file path
  -h, --help              Show this help message

Description:
  Rebuilds can_app.bin with configured CAN bus codes and packages it with the
  required auto_upgrade.txt trigger file into a ready-to-flash USB root structure.
EOF
}

DIRECT_PACKAGE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        -p|--preset)
            PRESET="$2"
            shift 2
            ;;
        -i|--input)
            STOCK_BIN="$2"
            shift 2
            ;;
        -o|--output-zip)
            ZIP_OUTPUT="$2"
            shift 2
            ;;
        -d|--direct)
            DIRECT_PACKAGE=1
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            print_usage
            exit 1
            ;;
    esac
done

if [[ ! -f "${STOCK_BIN}" ]]; then
    echo "[!] Error: Binary not found at ${STOCK_BIN}" >&2
    exit 1
fi

if [[ "${PRESET}" == "toyota_prado_150" ]]; then
    cat << 'EOF' >&2
======================================================================
 !! WARNING: toyota_prado_150 preset patches TWO CAN IDs, not one !!
======================================================================
This preset changes:
  - Mode 1 Entry 7 (SWC handler):            0x105 -> 0x3C4
  - Mode 1 Entry 4 (Reverse/parking handler): 0x185 -> 0x025

The second change is UNVERIFIED. docs/1.2_CANBUS.md (written before
this MCU tooling existed) lists 0x025/0x026 as *typical Prado SWC*
candidate IDs, not reverse-gear/parking-sensor IDs -- and explicitly
warns to verify CAN IDs against a live capture on your specific
vehicle before programming any MCU firmware. If 0x025 is actually SWC
traffic on this vehicle, steering-wheel button presses could
spuriously trigger the reverse-camera/parking-sensor handler while
driving.

There is also no known checksum/CRC covering the flash image itself
(only the runtime UART wire-protocol has one) -- this tooling has not
confirmed whether the resident bootloader validates the image any
further than YMODEM's own per-block transport CRC.

Do not flash this to a real vehicle without independently confirming
both CAN IDs via a live capture on your specific Prado first. See
docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md and docs/1.2_CANBUS.md.
======================================================================
EOF
fi

echo "======================================================================"
echo " Limcet STM32F105 MCU Firmware Update Builder"
echo "======================================================================"
echo "Input Binary:   ${STOCK_BIN}"
if [[ ${DIRECT_PACKAGE} -eq 0 ]]; then
    echo "Config File:    ${CONFIG_FILE}"
    if [[ -n "${PRESET}" ]]; then
        echo "Preset:         ${PRESET}"
    fi
else
    echo "Mode:           Direct Packaging (Pre-compiled Binary)"
fi

# Prepare staging directories
rm -rf "${OUTPUT_DIR}"
mkdir -p "${PACKAGE_DIR}"

REBUILT_BIN="${PACKAGE_DIR}/can_app.bin"
TRIGGER_FILE="${PACKAGE_DIR}/auto_upgrade.txt"

if [[ ${DIRECT_PACKAGE} -eq 1 ]]; then
    cp "${STOCK_BIN}" "${REBUILT_BIN}"
    echo "[+] Copied pre-compiled binary: ${REBUILT_BIN}"
else
    # Run Python rebuilder
    REBUILD_ARGS=("python3" "${SCRIPT_DIR}/mcu_rebuild.py" "${STOCK_BIN}" "-o" "${REBUILT_BIN}")
    if [[ -f "${CONFIG_FILE}" && -z "${PRESET}" ]]; then
        REBUILD_ARGS+=("--config" "${CONFIG_FILE}")
    fi
    if [[ -n "${PRESET}" ]]; then
        REBUILD_ARGS+=("--preset" "${PRESET}")
    fi
    "${REBUILD_ARGS[@]}"
fi

# Create trigger file (0 bytes)
touch "${TRIGGER_FILE}"
echo "[+] Created update trigger: ${TRIGGER_FILE} (0 bytes)"

# Create ZIP archive via Python's built-in zipfile
python3 -c "
import zipfile, os
with zipfile.ZipFile('${ZIP_OUTPUT}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('${PACKAGE_DIR}/auto_upgrade.txt', 'auto_upgrade.txt')
    z.write('${PACKAGE_DIR}/can_app.bin', 'can_app.bin')
"

echo "----------------------------------------------------------------------"
echo "[+] Update Package Created Successfully!"
echo "    Staging Directory: ${PACKAGE_DIR}/"
echo "    Files:"
echo "      - ${PACKAGE_DIR}/auto_upgrade.txt ($(stat -c%s "${TRIGGER_FILE}") bytes)"
echo "      - ${PACKAGE_DIR}/can_app.bin ($(stat -c%s "${REBUILT_BIN}") bytes, MD5: $(md5sum "${REBUILT_BIN}" | awk '{print $1}'))"
echo "    Deployable ZIP:    ${ZIP_OUTPUT}"
echo ""
echo "!! WARNING: This flashes the companion STM32 MCU over YMODEM. A bad image"
echo "!! can leave the CAN/steering-wheel-control subsystem non-functional or"
echo "!! misbehaving until re-flashed with a known-good binary. Keep a copy of"
echo "!! the original ${STOCK_BIN} before deploying a patched image."
echo ""
echo "Deployment Instructions:"
echo "  1. Copy 'auto_upgrade.txt' and 'can_app.bin' to the ROOT of a FAT32 USB flash drive."
echo "  2. Insert the USB drive into the CarPlay module USB port."
echo "  3. The system will detect 'auto_upgrade.txt' and automatically flash the MCU via YMODEM."
echo "======================================================================"
