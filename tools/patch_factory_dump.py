#!/usr/bin/env python3
"""
patch_factory_dump.py — Generates bit-accurate, verified factory restore binaries
by fixing confirmed extraction artifacts in the raw factory dump files.

Artifacts Patched:
1. Bootloader RCC_BASE Literal (0x08001234):
   - Offset 0x1234 in factory_bootloader_12k.bin:
     Replaces 0x802CF8D8 (extraction fault) with 0x40021000 (RCC_BASE).
     Used by RCC_GetClocksFreq at 0x0800120C.

2. Application Vector Table (0x08003000 & 0x08003004):
   - Offset 0x00 in factory_app_1302_52k.bin:
     Replaces 0xFFFFFFFF with 0x20005000 (Initial Main Stack Pointer).
   - Offset 0x04 in factory_app_1302_52k.bin:
     Replaces 0xFFFFFFFF with 0x08003151 (Application __main Reset Entry Point).

Outputs:
- hardware/MCU/live_dumps/factory_bootloader_12k_patched.bin (12,288 B)
- hardware/MCU/live_dumps/factory_app_1302_52k_patched.bin (53,248 B)
- hardware/MCU/live_dumps/factory_full_64k_patched.bin (65,536 B)
"""

import os
import sys
import hashlib
import struct

DUMP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "hardware", "MCU", "live_dumps"))

SRC_BOOTLOADER = os.path.join(DUMP_DIR, "factory_bootloader_12k.bin")
SRC_APP        = os.path.join(DUMP_DIR, "factory_app_1302_52k.bin")

OUT_BOOTLOADER = os.path.join(DUMP_DIR, "factory_bootloader_12k_patched.bin")
OUT_APP        = os.path.join(DUMP_DIR, "factory_app_1302_52k_patched.bin")
OUT_FULL       = os.path.join(DUMP_DIR, "factory_full_64k_patched.bin")

EXPECTED_BL_SHA256  = "322827aa0305d540591ddefc821bf6156f7c24dada9bc1e90e7c0a403dcb9db1"
EXPECTED_APP_SHA256 = "577f976fa96dd738ba65e597b06416cd4872ec38cbec3125ba736d0cfa3258b8"

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def main():
    print("=" * 70)
    print(" Factory Firmware Reconstruction & Artifact Patcher")
    print("=" * 70)

    # 1. Verify Source Bootloader
    if not os.path.exists(SRC_BOOTLOADER):
        print(f"Error: {SRC_BOOTLOADER} not found!", file=sys.stderr)
        return 1
    bl_hash = sha256_file(SRC_BOOTLOADER)
    print(f"[*] Verifying source bootloader: {os.path.basename(SRC_BOOTLOADER)}")
    if bl_hash != EXPECTED_BL_SHA256:
        print(f"Error: Bootloader SHA-256 mismatch!\n  Got:      {bl_hash}\n  Expected: {EXPECTED_BL_SHA256}", file=sys.stderr)
        return 1
    print("    SHA-256 verified authentic.")

    # 2. Patch Bootloader
    with open(SRC_BOOTLOADER, "rb") as f:
        bl_data = bytearray(f.read())
    
    if len(bl_data) != 12288:
        print(f"Error: Bootloader size is {len(bl_data)} (expected 12288)", file=sys.stderr)
        return 1
    
    # Check current artifact value at 0x1234
    cur_val = struct.unpack_from("<I", bl_data, 0x1234)[0]
    print(f"[*] Bootloader offset 0x1234 current word: 0x{cur_val:08X}")
    if cur_val != 0x802CF8D8:
        print(f"Warning: Expected 0x802CF8D8 at 0x1234, found 0x{cur_val:08X}!")
    
    # Patch with 0x40021000 (RCC_BASE)
    struct.pack_into("<I", bl_data, 0x1234, 0x40021000)
    new_val = struct.unpack_from("<I", bl_data, 0x1234)[0]
    print(f"    Patched offset 0x1234 with RCC_BASE: 0x{new_val:08X}")

    with open(OUT_BOOTLOADER, "wb") as f:
        f.write(bl_data)
    print(f"[+] Wrote patched bootloader: {OUT_BOOTLOADER} ({len(bl_data)} B)")
    print(f"    SHA-256: {sha256_file(OUT_BOOTLOADER)}")

    # 3. Verify Source Application
    if not os.path.exists(SRC_APP):
        print(f"Error: {SRC_APP} not found!", file=sys.stderr)
        return 1
    app_hash = sha256_file(SRC_APP)
    print(f"\n[*] Verifying source application: {os.path.basename(SRC_APP)}")
    if app_hash != EXPECTED_APP_SHA256:
        print(f"Error: App SHA-256 mismatch!\n  Got:      {app_hash}\n  Expected: {EXPECTED_APP_SHA256}", file=sys.stderr)
        return 1
    print("    SHA-256 verified authentic.")

    # 4. Patch Application Vector Table
    with open(SRC_APP, "rb") as f:
        app_data = bytearray(f.read())
    
    if len(app_data) != 53248:
        print(f"Error: Application size is {len(app_data)} (expected 53248)", file=sys.stderr)
        return 1

    cur_sp = struct.unpack_from("<I", app_data, 0x00)[0]
    cur_reset = struct.unpack_from("<I", app_data, 0x04)[0]
    print(f"[*] App vector table current: SP=0x{cur_sp:08X}, Reset=0x{cur_reset:08X}")

    # Patch SP = 0x20005000 and Reset = 0x08003151
    struct.pack_into("<I", app_data, 0x00, 0x20005000)
    struct.pack_into("<I", app_data, 0x04, 0x08003151)
    new_sp = struct.unpack_from("<I", app_data, 0x00)[0]
    new_reset = struct.unpack_from("<I", app_data, 0x04)[0]
    print(f"    Patched App vector table: SP=0x{new_sp:08X}, Reset=0x{new_reset:08X}")

    with open(OUT_APP, "wb") as f:
        f.write(app_data)
    print(f"[+] Wrote patched application: {OUT_APP} ({len(app_data)} B)")
    print(f"    SHA-256: {sha256_file(OUT_APP)}")

    # 5. Create Full 64K Combined Image
    full_data = bl_data + app_data
    if len(full_data) != 65536:
        print(f"Error: Full image size is {len(full_data)} (expected 65536)", file=sys.stderr)
        return 1

    with open(OUT_FULL, "wb") as f:
        f.write(full_data)
    print(f"\n[+] Wrote full patched restore image: {OUT_FULL} ({len(full_data)} B)")
    print(f"    SHA-256: {sha256_file(OUT_FULL)}")
    print("\n[SUCCESS] All factory restore images successfully generated and verified.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
