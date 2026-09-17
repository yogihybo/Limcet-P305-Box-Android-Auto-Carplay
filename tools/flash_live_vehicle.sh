#!/usr/bin/env bash
# Interactive safety gate for tools/flash_live_safe.tcl.
#
# flash_live_safe.tcl unlocks RDP (triggering a full mass erase) and
# reprograms the STM32F105 companion MCU. Run against the wrong target --
# or with a stale/corrupt image -- this can leave the head unit's MCU
# unbootable, holding the ArkMicro SoC in permanent hardware reset (PB14
# low) until the BOOT0 mask-ROM recovery path in
# docs/LIVE_VEHICLE_FLASH_AND_RECOVERY_PROTOCOL.md is used.
#
# This wrapper is the only supported way to run that script: it verifies
# the image before anything touches the target, and requires the operator
# to type an explicit confirmation phrase. flash_live_safe.tcl itself
# refuses to run without the guard file this script creates.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

IMAGE="hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin"
EXPECTED_SHA256="b385f87cbb8a183add3507c45fb6006cbad3798cbd00805338f15cc50704c161"
EXPECTED_SIZE=65536
CONFIRM_PHRASE="FLASH LIVE VEHICLE"
OCD_CFG="tools/pico_stm32.cfg"
OCD_SCRIPT="tools/flash_live_safe.tcl"

echo "=================================================================="
echo " DANGER: LIVE VEHICLE MCU REFLASH"
echo "=================================================================="
echo " This unlocks Readout Protection (RDP), which triggers a full"
echo " silicon mass-erase of the STM32F105 companion MCU, then reprograms"
echo " it -- while it is installed in the actual vehicle head unit, not"
echo " the spare test board."
echo
echo " A failed or interrupted flash requires the BOOT0 hard-recovery"
echo " path in docs/LIVE_VEHICLE_FLASH_AND_RECOVERY_PROTOCOL.md section 6."
echo "=================================================================="
echo

if [[ ! -f "$IMAGE" ]]; then
    echo "ABORT: image not found: $IMAGE" >&2
    exit 1
fi

actual_size=$(stat -c%s "$IMAGE")
if [[ "$actual_size" -ne "$EXPECTED_SIZE" ]]; then
    echo "ABORT: image is $actual_size bytes, expected exactly $EXPECTED_SIZE." >&2
    echo "Refusing to flash a truncated or otherwise wrong-size image." >&2
    exit 1
fi

actual_sha256=$(sha256sum "$IMAGE" | awk '{print $1}')
if [[ "$actual_sha256" != "$EXPECTED_SHA256" ]]; then
    echo "ABORT: image SHA-256 does not match the known-good hash." >&2
    echo "  expected: $EXPECTED_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    echo "Refusing to flash an image that wasn't the one this was reviewed against." >&2
    exit 1
fi
echo "Image OK: $IMAGE"
echo "  size:   $actual_size bytes"
echo "  sha256: $actual_sha256"
echo

read -r -p "Is the SWD probe connected to the VEHICLE unit, not the spare test board? [yes/N] " target_ans
if [[ "$target_ans" != "yes" ]]; then
    echo "Aborted -- answer was not exactly 'yes'."
    exit 1
fi

echo
echo "Type the following phrase exactly to proceed:"
echo "  $CONFIRM_PHRASE"
read -r -p "> " typed_phrase
if [[ "$typed_phrase" != "$CONFIRM_PHRASE" ]]; then
    echo "ABORT: confirmation phrase did not match." >&2
    exit 1
fi

guard_file="$(mktemp /tmp/flash_live_vehicle_confirmed.XXXXXX)"
cleanup() { rm -f "$guard_file"; }
trap cleanup EXIT

echo
echo "Confirmed. Invoking OpenOCD..."
echo
FLASH_LIVE_GUARD_FILE="$guard_file" \
FLASH_LIVE_EXPECTED_SHA256="$EXPECTED_SHA256" \
FLASH_LIVE_EXPECTED_SIZE="$EXPECTED_SIZE" \
    openocd -f "$OCD_CFG" -f "$OCD_SCRIPT"
