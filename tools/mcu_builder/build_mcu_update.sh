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
DO_OBFUSCATE=1
DIRECT_PACKAGE=0

print_usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -c, --config FILE       Path to custom CAN JSON config file
  -p, --preset PRESET     Vehicle preset (e.g. toyota_prado_150)
  -i, --input BINARY      Input application binary (default: hardware/MCU/can_app.bin)
  -o, --output-zip ZIP    Output zip file path
  -d, --direct            Direct packaging (pre-compiled binary, skip python CAN patcher)
  --no-obfuscate          Do not apply wire obfuscation (leave binary raw/plaintext)
  -h, --help              Show this help message

Description:
  Packages STM32 companion MCU application firmware with the auto_upgrade.txt
  trigger file for USB flashing via the head unit's libMcuCenter.so daemon.
  By default, applies the factory bootloader byte obfuscation transform:
    wire[i] = ~(flash[i] - T[i % 128]) & 0xFF
EOF
}

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
        --no-obfuscate)
            DO_OBFUSCATE=0
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
echo "Obfuscation:    $([[ ${DO_OBFUSCATE} -eq 1 ]] && echo 'Enabled (OEM bootloader wire format)' || echo 'Disabled (Raw plaintext)')"

# Prepare staging directories
rm -rf "${OUTPUT_DIR}"
mkdir -p "${PACKAGE_DIR}"

RAW_STAGING="${OUTPUT_DIR}/can_app_plain.bin"
REBUILT_BIN="${PACKAGE_DIR}/can_app.bin"
TRIGGER_FILE="${PACKAGE_DIR}/auto_upgrade.txt"

if [[ ${DIRECT_PACKAGE} -eq 1 ]]; then
    cp "${STOCK_BIN}" "${RAW_STAGING}"
    echo "[+] Staged pre-compiled binary: ${RAW_STAGING}"
else
    # Run Python rebuilder
    REBUILD_ARGS=("python3" "${SCRIPT_DIR}/mcu_rebuild.py" "${STOCK_BIN}" "-o" "${RAW_STAGING}")
    if [[ -f "${CONFIG_FILE}" && -z "${PRESET}" ]]; then
        REBUILD_ARGS+=("--config" "${CONFIG_FILE}")
    fi
    if [[ -n "${PRESET}" ]]; then
        REBUILD_ARGS+=("--preset" "${PRESET}")
    fi
    "${REBUILD_ARGS[@]}"
fi

# Apply obfuscation if enabled
if [[ ${DO_OBFUSCATE} -eq 1 ]]; then
    echo "[*] Applying OEM wire-obfuscation transform (table T @ 0x08001F72)..."
    python3 "${SCRIPT_DIR}/mcu_obfuscate.py" --obfuscate "${RAW_STAGING}" -o "${REBUILT_BIN}"
else
    cp "${RAW_STAGING}" "${REBUILT_BIN}"
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
echo "      - ${PACKAGE_DIR}/can_app.bin ($(stat -c%s "${REBUILT_BIN}") bytes, SHA256: $(sha256sum "${REBUILT_BIN}" | awk '{print $1}'))"
echo "    Deployable ZIP:    ${ZIP_OUTPUT}"
echo ""
echo "Deployment Instructions:"
echo "  1. Copy 'auto_upgrade.txt' and 'can_app.bin' to the ROOT of a FAT32 USB flash drive."
echo "  2. Insert the USB drive into the CarPlay module USB port."
echo "  3. The system daemon libMcuCenter.so will detect 'auto_upgrade.txt' and flash"
echo "     the STM32 MCU over /dev/ttyHS0 using the OEM 0x2E protocol (E1/E2/E3/E4)."
echo "======================================================================"
