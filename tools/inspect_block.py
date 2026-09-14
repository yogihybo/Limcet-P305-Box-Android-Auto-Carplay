#!/usr/bin/env python3
import sys
import capstone

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)

with open('hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_raw.bin', 'rb') as f:
    app = f.read()

def inspect_block(b):
    b_addr = 0x08003000 + b * 512
    idx = b_addr - 0x08003000
    print(f"============================== BLOCK {b:2d} (0x{b_addr:08X}) ==============================")
    
    # Region 1: 0x00 to 0x08
    print("--- REGION 1: +0x00 to +0x08 (Gaps at +0x00, +0x04) ---")
    chunk_pre = app[idx-16:idx]
    for insn in md.disasm(chunk_pre, b_addr-16):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")
    print(f"  [ GAP 0x{b_addr:08X} - 0x{b_addr+8:08X} (8 bytes) ]")
    chunk_post = app[idx+8:idx+0x1C]
    for insn in md.disasm(chunk_post, b_addr+8):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")
        
    # Region 2: +0x1C to +0x2C
    print("\n--- REGION 2: +0x1C to +0x2C (Gaps at +0x1C, +0x20, +0x24, +0x28) ---")
    chunk_pre2 = app[idx+0x10:idx+0x1C]
    for insn in md.disasm(chunk_pre2, b_addr+0x10):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")
    print(f"  [ GAP 0x{b_addr+0x1C:08X} - 0x{b_addr+0x2C:08X} (16 bytes) ]")
    chunk_post2 = app[idx+0x2C:idx+0x34]
    for insn in md.disasm(chunk_post2, b_addr+0x2C):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")

    # Region 3: +0x34 to +0x38
    print("\n--- REGION 3: +0x34 to +0x38 (Gap at +0x34) ---")
    chunk_pre3 = app[idx+0x28:idx+0x34]
    for insn in md.disasm(chunk_pre3, b_addr+0x28):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")
    print(f"  [ GAP 0x{b_addr+0x34:08X} - 0x{b_addr+0x38:08X} (4 bytes) ]")
    chunk_post3 = app[idx+0x38:idx+0x48]
    for insn in md.disasm(chunk_post3, b_addr+0x38):
        print(f"  0x{insn.address:08X}: {insn.mnemonic:<8} {insn.op_str} ({insn.bytes.hex()})")

if __name__ == '__main__':
    for arg in sys.argv[1:]:
        inspect_block(int(arg))
