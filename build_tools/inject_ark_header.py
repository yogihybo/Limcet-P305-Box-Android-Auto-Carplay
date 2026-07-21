#!/usr/bin/env python3
"""
build_tools/inject_ark_header.py — Inject ARK U-Boot header fields into compiled u-boot.bin

The stock Prado/ARK U-Boot binary has a proprietary header embedded in the
exception vector table area (offsets 0x3c–0x5c). The open-source U-Boot build
does NOT produce this header, so it must be injected as a post-build step.

Header structure (from stock binary reverse-engineering):
  0x3c: 0x12345678  ARK magic
  0x40: 0x00030000  Load address (fixed)
  0x44: <ep>        Entry point (from ELF)
  0x48: <ep>        Entry point (duplicate)
  0x4c: <bss_end>   BSS end address (from ELF)
  0x50: <filesize>  File size of u-boot.bin

Usage:
  python3 build_tools/inject_ark_header.py u-boot.bin u-boot-ark.bin
  python3 build_tools/inject_ark_header.py u-boot.bin u-boot-ark.bin --verify

The input binary must be the flat binary (u-boot.bin), and the companion ELF
(u-boot, no extension) must exist in the same directory.
"""

import struct
import subprocess
import sys
import os
import argparse


ARK_MAGIC    = 0x12345678
LOAD_ADDRESS = 0x00030000

# Offsets in the binary where header fields live
OFF_MAGIC    = 0x3c
OFF_LOAD     = 0x40
OFF_EP       = 0x44
OFF_EP_DUP   = 0x48
OFF_BSS_END  = 0x4c
OFF_FILESIZE = 0x50


def get_elf_entry(elf_path: str) -> int:
    """Get the entry point for the ARK header.

    The stock ARK binaries use board_init_r as the EP in the ARK header
    (not _start / the ELF entry point). Verified by comparing stock Prado,
    P306, and Holden firmware dumps.
    """
    try:
        out = subprocess.check_output(
            ['arm-linux-gnueabihf-nm', elf_path],
            stderr=subprocess.DEVNULL
        )
    except FileNotFoundError:
        raise SystemExit("ERROR: arm-linux-gnueabihf-nm not found. Install gcc-arm-linux-gnueabihf.")
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"ERROR: nm failed on {elf_path}: {e}")

    for line in out.decode().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == 'board_init_r':
            return int(parts[0], 16)

    # Fallback: use ELF entry point from readelf
    print("WARNING: board_init_r not found in symbol table, falling back to ELF entry point")
    try:
        out2 = subprocess.check_output(
            ['arm-linux-gnueabihf-readelf', '-h', elf_path],
            stderr=subprocess.DEVNULL
        )
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"ERROR: readelf failed on {elf_path}: {e}")
    for line in out2.decode().splitlines():
        if 'Entry point' in line:
            return int(line.split()[-1], 16)
    raise SystemExit(f"ERROR: Cannot determine entry point from {elf_path}")


def get_bss_end(elf_path: str) -> int:
    """Read BSS section end address from ELF (addr + size)."""
    try:
        out = subprocess.check_output(
            ['arm-linux-gnueabihf-readelf', '-S', '--wide', elf_path],
            stderr=subprocess.DEVNULL
        )
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"ERROR: readelf -S failed on {elf_path}: {e}")

    bss_addr = bss_size = 0
    for line in out.decode().splitlines():
        # Typical format: [ N] .bss  NOBITS  addr  off  size  ES  Flg  Lk  Inf  Al
        if ' .bss ' in line or (line.strip().startswith('[') and '.bss' in line):
            parts = line.split()
            # Skip the index field [N]
            idx = next((i for i, p in enumerate(parts) if p == '.bss'), None)
            if idx is not None and idx + 4 < len(parts):
                try:
                    bss_addr = int(parts[idx + 2], 16)
                    bss_size = int(parts[idx + 4], 16)
                except ValueError:
                    continue

    if bss_addr == 0:
        print("WARNING: Could not find .bss section, BSS end will be 0")
    return bss_addr + bss_size


def verify_binary(data: bytes, ep: int, bss_end: int) -> bool:
    """Verify that the header fields look correct."""
    ok = True
    magic    = struct.unpack_from('<I', data, OFF_MAGIC)[0]
    load     = struct.unpack_from('<I', data, OFF_LOAD)[0]
    ep_field = struct.unpack_from('<I', data, OFF_EP)[0]
    size     = struct.unpack_from('<I', data, OFF_FILESIZE)[0]

    print(f"\n=== Header Verification ===")
    print(f"  [0x{OFF_MAGIC:02x}] magic    = {magic:#010x}  {'OK' if magic == ARK_MAGIC else 'FAIL expected ' + hex(ARK_MAGIC)}")
    print(f"  [0x{OFF_LOAD:02x}] load     = {load:#010x}  {'OK' if load == LOAD_ADDRESS else 'FAIL expected ' + hex(LOAD_ADDRESS)}")
    print(f"  [0x{OFF_EP:02x}] ep       = {ep_field:#010x}  {'OK' if ep_field == ep else 'FAIL expected ' + hex(ep)}")
    print(f"  [0x{OFF_FILESIZE:02x}] filesize = {size:#010x}  {'OK' if size == len(data) else 'FAIL expected ' + hex(len(data))}")

    # Reset vector check: should branch to reset handler
    reset_vec = struct.unpack_from('<I', data, 0)[0]
    if (reset_vec & 0xFF000000) == 0xEA000000:
        branch_offset = reset_vec & 0xFFFFFF
        _start_offset = 8 + branch_offset * 4
        print(f"\n  Reset vector: {reset_vec:#010x} -> branches to file offset {_start_offset:#x}")
    else:
        print(f"\n  WARNING: Reset vector {reset_vec:#010x} doesn't look like an ARM branch!")
        ok = False

    print(f"\n  Binary size: {len(data):#x} ({len(data)} bytes)")
    print(f"  Load + size = {LOAD_ADDRESS + len(data):#010x}")

    if magic != ARK_MAGIC or load != LOAD_ADDRESS or ep_field != ep or size != len(data):
        ok = False

    return ok


def main():
    parser = argparse.ArgumentParser(description='Inject ARK U-Boot header into compiled binary')
    parser.add_argument('input',  help='Input binary (u-boot.bin)')
    parser.add_argument('output', help='Output binary with ARK header injected')
    parser.add_argument('--elf',  help='ELF file path (default: input with .bin removed)')
    parser.add_argument('--verify', action='store_true', help='Verify an existing binary without modifying it')
    args = parser.parse_args()

    # Determine ELF path
    elf_path = args.elf
    if elf_path is None:
        if args.input.endswith('.bin'):
            elf_path = args.input[:-4]
        else:
            elf_path = args.input + '_elf'

    if not os.path.exists(args.input):
        raise SystemExit(f"ERROR: Input file not found: {args.input}")

    if not os.path.exists(elf_path) and not args.verify:
        raise SystemExit(
            f"ERROR: ELF file not found: {elf_path}\n"
            f"       Build with: make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)\n"
            f"       Or specify with --elf /path/to/u-boot"
        )

    # Load binary
    with open(args.input, 'rb') as f:
        data = bytearray(f.read())

    filesize = len(data)
    print(f"Input:    {args.input} ({filesize} bytes = {filesize:#x})")

    if args.verify:
        print("\n[Verify mode - reading existing header fields]")
        ep_field  = struct.unpack_from('<I', data, OFF_EP)[0]
        bss_field = struct.unpack_from('<I', data, OFF_BSS_END)[0]
        verify_binary(bytes(data), ep_field, bss_field)
        return

    # Get info from ELF
    ep      = get_elf_entry(elf_path)
    bss_end = get_bss_end(elf_path)

    print(f"ELF:      {elf_path}")
    print(f"  Entry point: {ep:#010x}")
    print(f"  BSS end:     {bss_end:#010x}")

    # Warn if the offsets we're about to write contain unexpected values
    magic_current = struct.unpack_from('<I', data, OFF_MAGIC)[0]
    if magic_current == ARK_MAGIC:
        print(f"\nWARNING: ARK magic already present at 0x{OFF_MAGIC:02x} -- overwriting.")
    elif magic_current not in (0xdeadbeef, 0x0badc0de):
        print(f"\nWARNING: Unexpected value at 0x{OFF_MAGIC:02x}: {magic_current:#010x}")
        print(f"         Expected 0xdeadbeef (compiler padding). Proceeding anyway.")

    # Inject header fields
    struct.pack_into('<I', data, OFF_MAGIC,    ARK_MAGIC)
    struct.pack_into('<I', data, OFF_LOAD,     LOAD_ADDRESS)
    struct.pack_into('<I', data, OFF_EP,       ep)
    struct.pack_into('<I', data, OFF_EP_DUP,   ep)
    struct.pack_into('<I', data, OFF_BSS_END,  bss_end)
    struct.pack_into('<I', data, OFF_FILESIZE, filesize)

    # Write output
    with open(args.output, 'wb') as f:
        f.write(data)

    print(f"\nOutput:   {args.output}")
    ok = verify_binary(bytes(data), ep, bss_end)
    print(f"\n{'U-boot header injection successful' if ok else 'Header injection had issues - check above'}")

    if ok:
        print(f"\nNext steps:")
        print(f"  1. Copy to SD card: cp {args.output} /mnt/sdcard/UBOOT.BIN")
        print(f"  2. Insert SD card and power on")
        print(f"  3. Watch serial console for: U-Boot 20xx.xx")


if __name__ == '__main__':
    main()
