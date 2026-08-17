# Limcet STM32 MCU Reverse Engineering & Firmware Rebuilder Toolset

This toolkit provides tools for analyzing, decompiling, patching, and rebuilding the companion STM32F105 MCU firmware (`can_app.bin`) for Limcet CarPlay / Android Auto head units (e.g. Limcet Box-P305 / Box-P306).

---

## 1. Overview & Directory Structure

- `mcu_decompile.py`: Disassembler and structural analyzer for `can_app.bin`. Extracts vector tables, active IRQs, UART command dispatch table, CAN mode dispatch tables, and handler disassemblies.
- `mcu_rebuild.py`: Firmware patcher and builder. Modifies CAN IDs, mode dispatch tables, and opcode bitmasks to customize vehicle CAN profiles (e.g. Toyota Prado SWC `0x3C4`).
- `build_mcu_update.sh`: Automated script to rebuild the MCU firmware and package it with `auto_upgrade.txt` into a FAT32 USB-ready update package.
- `can_config_template.json`: JSON configuration file mapping CAN IDs and button bitmasks.

---

## 2. Quick Start: Rebuilding for Toyota Prado

To build a ready-to-flash USB update package configured for the Toyota Prado 150 preset (SWC `0x3C4`):

```bash
./tools/mcu_builder/build_mcu_update.sh --preset toyota_prado_150
```

The output files are generated in `tools/mcu_builder/output/`:
- `output/usb_root/auto_upgrade.txt` (0-byte trigger flag)
- `output/usb_root/can_app.bin` (31,996-byte patched STM32 firmware)
- `output/mcu_update_package.zip` (Deployable zip archive)

---

## 3. Flash Deployment via USB

1. Copy both `auto_upgrade.txt` and `can_app.bin` from `tools/mcu_builder/output/usb_root/` directly to the **ROOT directory** of a FAT32-formatted USB flash drive.
2. Turn on the vehicle and wait for the CarPlay head unit to boot.
3. Insert the USB drive into the module's USB port.
4. The system daemon `libMcuCenter.so` detects `auto_upgrade.txt` and initiates YMODEM transfer over `/dev/ttyHS0` to reflash the STM32 MCU.

---

## 4. Custom CAN Bus Configuration

To customize specific CAN IDs or map new vehicle signals, edit `can_config_template.json` or pass custom parameters:

```bash
# Decompile and inspect current binary
python3 tools/mcu_builder/mcu_decompile.py hardware/MCU/can_app.bin --json-out mcu_report.json

# Patch specific CAN ID (e.g. Mode 1, Index 7 -> 0x3C4)
python3 tools/mcu_builder/mcu_rebuild.py hardware/MCU/can_app.bin -o output/can_app.bin --set-id 1 7 0x3c4

# Build with custom JSON configuration
./tools/mcu_builder/build_mcu_update.sh --config path/to/custom_config.json
```
