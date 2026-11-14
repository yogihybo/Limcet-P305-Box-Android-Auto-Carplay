# Limcet STM32 MCU Reverse Engineering & Firmware Rebuilder Toolset

This toolkit provides tools for analyzing, decompiling, patching, and rebuilding the companion STM32F105 MCU firmware (`can_app.bin`) for Limcet CarPlay / Android Auto head units (e.g. Limcet Box-P305 / Box-P306).

> **⚠️ Read before flashing anything built here to a real vehicle:**
> - `--preset toyota_prado_150` patches **two** CAN IDs. Only one (the SWC ID,
>   `0x105 -> 0x3C4`) has real basis in this project's research; the other
>   (`0x185 -> 0x025`, the reverse/parking-sensor handler) is **unverified** and
>   may conflict with SWC traffic per `docs/1.2_CANBUS.md`. See
>   `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s warning banner for the full
>   explanation before using this preset.
> - No image-level checksum/CRC verification is confirmed to exist for
>   `can_app.bin` — only the runtime UART protocol has one. Keep a copy of the
>   stock binary before flashing a patched image.

---

## 1. Overview & Directory Structure

- `mcu_obfuscate.py`: Obfuscates plaintext firmware images into the OEM bootloader wire format (`wire[i] = ~(flash[i] - T[i % 128]) & 0xFF`), and de-obfuscates wire captures.
- `mcu_decompile.py`: Disassembler and structural analyzer for `can_app.bin`. Extracts vector tables, active IRQs, UART command dispatch table, CAN mode dispatch tables, and handler disassemblies.
- `mcu_rebuild.py`: Firmware patcher and builder. Modifies CAN IDs, mode dispatch tables, and opcode bitmasks to customize vehicle CAN profiles (e.g. Toyota Prado SWC `0x3C4`).
- `build_mcu_update.sh`: Automated script to rebuild/stage MCU firmware, apply OEM wire obfuscation, and package with `auto_upgrade.txt` into a FAT32 USB-ready update package.
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

**⚠️ This flashes the companion STM32 MCU that drives CAN bus / steering-wheel
controls. A bad image can leave that subsystem non-functional or misbehaving
until re-flashed with a known-good binary.** The known-good image for this unit is the
reconstructed factory application,
`hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin`:

```bash
./tools/mcu_builder/build_mcu_update.sh -d -i hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin
```

**`hardware/MCU/can_app.bin` is not this unit's firmware** -- it is the DCn32-VOLVO
reference image, linked at `0x08004000` for a different product. The packager now has
no default input and refuses any image not linked for the `0x08003000` application base
(`tools/mcu_builder/check_app_image.py`), so it can't be packaged by accident.

1. Copy both `auto_upgrade.txt` and `can_app.bin` from `tools/mcu_builder/output/usb_root/` directly to the **ROOT directory** of a FAT32-formatted USB flash drive.
2. Turn on the vehicle and wait for the CarPlay head unit to boot.
3. Insert the USB drive into the module's USB port.
4. The system daemon `libMcuCenter.so` detects `auto_upgrade.txt`, triggers MCU reboot into bootloader (`CMD 0xE1`), and transmits `can_app.bin` over `/dev/ttyHS0` using the OEM `0x2E` framed protocol (`E1`/`E2`/`E3`/`E4`). The bootloader de-obfuscates each byte and flashes the 116 KB application region.

---

## 4. Custom CAN Bus Configuration

**These tools patch the DCn32 reference image's CAN dispatch tables.** That image can't
run on this unit, so anything they produce from it is refused by the packager; they are
kept for analysing the DCn32 tables only.

To customize specific CAN IDs or map new vehicle signals, edit `can_config_template.json` or pass custom parameters:

```bash
# Decompile and inspect current binary
python3 tools/mcu_builder/mcu_decompile.py hardware/MCU/can_app.bin --json-out mcu_report.json

# Patch specific CAN ID (e.g. Mode 1, Index 7 -> 0x3C4)
python3 tools/mcu_builder/mcu_rebuild.py hardware/MCU/can_app.bin -o output/can_app.bin --set-id 1 7 0x3c4

# Build with custom JSON configuration
./tools/mcu_builder/build_mcu_update.sh --config path/to/custom_config.json
```
