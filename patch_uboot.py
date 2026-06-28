#!/usr/bin/env python3
"""
patch_uboot.py - Patch compiled-in environment in ARK1680 U-Boot binary

The U-Boot binary contains a small compiled-in env block used as fallback
when the primary env storage (NAND mtd3) fails its CRC check. This script:
  1. Finds and patches key=value pairs in that block
  2. Optionally redirects the NAND env read offset to an invalid address,
     forcing CRC failure so U-Boot uses the patched compiled-in defaults

Examples:
  # Inspect compiled-in env
  python patch_uboot.py -i uboot.bin --dump-env

  # Find NAND env offset candidates (run before --nand-offset-index)
  python patch_uboot.py -i uboot.bin --find-nand-offset

  # Apply SD boot preset and force NAND env fallback (index 0 most likely)
  python patch_uboot.py -i uboot.bin -o uboot_sdboot.bin --mode sdboot --nand-offset-index 0

  # Custom root device
  python patch_uboot.py -i uboot.bin -o uboot_sdboot.bin --mode sdboot --root /dev/mmcblk1p2 --nand-offset-index 0

  # Manual env patches only (no NAND offset change)
  python patch_uboot.py -i uboot.bin -o uboot_patched.bin --set bootcmd="run mmcboot" --set bootdelay=3

  # Dry run — show what would change without writing
  python patch_uboot.py -i uboot.bin --mode sdboot --nand-offset-index 0 --dry-run
"""

import argparse
import struct
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Compiled-in env block
# ---------------------------------------------------------------------------

def find_env_block(data: bytes) -> int:
    """
    Find the compiled-in env block offset by locating 'bootcmd=' preceded
    by a null byte (start of a new env entry) or at offset 0.
    Returns -1 if not found.
    """
    marker = b'bootcmd='
    pos = 0
    while True:
        idx = data.find(marker, pos)
        if idx == -1:
            return -1
        if idx == 0 or data[idx - 1] == 0:
            return idx
        pos = idx + 1


def parse_env(data: bytes, offset: int, max_size: int = 4096) -> dict:
    """Parse null-terminated key=value pairs from env block into an ordered dict."""
    env = {}
    pos = offset
    end = min(offset + max_size, len(data))
    while pos < end:
        null = data.find(b'\x00', pos, end)
        if null == -1 or null == pos:
            break
        entry = data[pos:null].decode('ascii', errors='replace')
        if '=' in entry:
            k, v = entry.split('=', 1)
            env[k] = v
        pos = null + 1
    return env


def serialize_env(env: dict) -> bytes:
    """Serialize env dict to null-terminated byte string (double-null terminated)."""
    entries = [f'{k}={v}'.encode('ascii') for k, v in env.items()]
    return b'\x00'.join(entries) + b'\x00\x00'


def patch_env_block(data: bytearray, offset: int, patches: dict,
                    max_size: int = 4096) -> bool:
    """
    Merge patches into the existing compiled-in env block and write back.
    Returns False if the result is too large for the block.
    """
    existing = parse_env(bytes(data), offset, max_size)
    existing.update(patches)
    serialized = serialize_env(existing)
    if len(serialized) > max_size:
        print(f"ERROR: patched env ({len(serialized)} B) exceeds block size ({max_size} B)")
        print("       Use --env-block-size if the block is larger than default 4096 B")
        return False
    padded = serialized + b'\x00' * (max_size - len(serialized))
    data[offset:offset + max_size] = padded
    return True


# ---------------------------------------------------------------------------
# NAND env offset
# ---------------------------------------------------------------------------

# Default NAND env partition start on Prado (mtd3, 384 KiB = 0x60000)
DEFAULT_NAND_ENV_OFFSET = 0x00060000
# Value that will cause the NAND read to fail (beyond any real NAND range)
INVALID_NAND_OFFSET = 0xFF000000


def find_nand_offset_candidates(data: bytes,
                                target: int = DEFAULT_NAND_ENV_OFFSET) -> list:
    """
    Find all little-endian 32-bit occurrences of `target` in the binary.
    Returns list of dicts with 'offset' and 'context' keys.
    """
    needle = struct.pack('<I', target)
    candidates = []
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx == -1:
            break
        ctx_start = max(0, idx - 16)
        ctx_bytes = data[ctx_start:idx + 20]
        candidates.append({
            'offset': idx,
            'context': ctx_bytes.hex(' '),
        })
        pos = idx + 1
    return candidates


def patch_nand_offset(data: bytearray, binary_offset: int,
                      new_value: int = INVALID_NAND_OFFSET):
    """Overwrite the 4-byte LE word at binary_offset with new_value."""
    struct.pack_into('<I', data, binary_offset, new_value)


# ---------------------------------------------------------------------------
# Preset profiles
# ---------------------------------------------------------------------------

PRESETS = {
    'sdboot': {
        'description': 'Boot from SD card — ext4 rootfs on /dev/mmcblk0p2',
        'env': {
            'bootcmd':     'run mmcboot',
            'setbootargs': (
                'setenv bootargs console=ttyS0,115200n8 mem=180M '
                'root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw'
            ),
            'bootdelay':   '3',
        },
    },
}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Patch compiled-in environment in ARK1680 U-Boot binary',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument('-i', '--input', required=True,
                    help='Input U-Boot binary')
    ap.add_argument('-o', '--output',
                    help='Output patched binary (default: <input>_patched.bin)')
    ap.add_argument('--dump-env', action='store_true',
                    help='Print compiled-in env block and exit')
    ap.add_argument('--find-nand-offset', action='store_true',
                    help='List NAND env offset candidates and exit')
    ap.add_argument('--mode', choices=list(PRESETS.keys()),
                    help='Apply a preset patch profile')
    ap.add_argument('--root', default='/dev/mmcblk0p2', metavar='DEVICE',
                    help='Root device for sdboot mode (default: /dev/mmcblk0p2)')
    ap.add_argument('--set', metavar='KEY=VALUE', action='append', dest='setenv',
                    help='Set a compiled-in env variable (repeatable, applied after --mode)')
    ap.add_argument('--nand-offset-index', type=int, metavar='N',
                    help='Candidate index from --find-nand-offset to redirect to 0xFF000000')
    ap.add_argument('--nand-offset-value', default='0xFF000000', metavar='HEX',
                    help='Replacement value for NAND env offset (default: 0xFF000000)')
    ap.add_argument('--env-block-size', type=int, default=4096, metavar='BYTES',
                    help='Max compiled-in env block size in bytes (default: 4096)')
    ap.add_argument('--dry-run', action='store_true',
                    help='Show changes without writing output file')
    args = ap.parse_args()

    data_ro = Path(args.input).read_bytes()
    data = bytearray(data_ro)

    # Locate compiled-in env block
    env_offset = find_env_block(data_ro)
    if env_offset == -1:
        print("ERROR: compiled-in env block not found (no 'bootcmd=' preceded by null byte)")
        sys.exit(1)
    print(f"Compiled-in env block: 0x{env_offset:08X}")

    # --dump-env
    if args.dump_env:
        env = parse_env(data_ro, env_offset, args.env_block_size)
        print("\nCompiled-in environment:")
        for k, v in env.items():
            print(f"  {k}={v}")
        return

    # --find-nand-offset
    candidates = find_nand_offset_candidates(data_ro)
    if args.find_nand_offset:
        if not candidates:
            print(f"\nNo occurrences of 0x{DEFAULT_NAND_ENV_OFFSET:08X} found in binary.")
        else:
            print(f"\nNAND env offset (0x{DEFAULT_NAND_ENV_OFFSET:08X}) candidates:")
            for i, c in enumerate(candidates):
                print(f"  [{i}] binary 0x{c['offset']:08X}  hex: {c['context']}")
            print("\nPass --nand-offset-index N to patch one of these.")
        return

    # Collect env patches
    patches = {}

    if args.mode:
        preset = PRESETS[args.mode]
        print(f"\nMode: {args.mode} — {preset['description']}")
        patches.update(preset['env'])
        # Apply --root override to sdboot setbootargs
        if args.mode == 'sdboot' and args.root != '/dev/mmcblk0p2':
            patches['setbootargs'] = (
                f"setenv bootargs console=ttyS0,115200n8 mem=180M "
                f"root={args.root} rootfstype=ext4 rootwait rw"
            )

    if args.setenv:
        for item in args.setenv:
            if '=' not in item:
                print(f"ERROR: --set requires KEY=VALUE, got: {item!r}")
                sys.exit(1)
            k, v = item.split('=', 1)
            patches[k] = v

    if not patches and args.nand_offset_index is None:
        print("\nNothing to patch. Use --mode, --set, or --nand-offset-index.")
        ap.print_help()
        sys.exit(1)

    # Show and apply env patches
    if patches:
        orig = parse_env(data_ro, env_offset, args.env_block_size)
        print("\nEnv patches:")
        for k, v in patches.items():
            old = orig.get(k, '<not present>')
            print(f"  {k}:")
            print(f"    before: {old}")
            print(f"    after:  {v}")
        if not args.dry_run:
            if not patch_env_block(data, env_offset, patches, args.env_block_size):
                sys.exit(1)

    # Show and apply NAND offset patch
    if args.nand_offset_index is not None:
        if not candidates:
            print(f"\nERROR: no NAND env offset candidates found. Run --find-nand-offset first.")
            sys.exit(1)
        if args.nand_offset_index >= len(candidates):
            print(f"\nERROR: index {args.nand_offset_index} out of range "
                  f"({len(candidates)} candidates). Run --find-nand-offset to list them.")
            sys.exit(1)
        c = candidates[args.nand_offset_index]
        new_val = int(args.nand_offset_value, 16)
        print(f"\nNAND offset patch at binary 0x{c['offset']:08X}:")
        print(f"  0x{DEFAULT_NAND_ENV_OFFSET:08X} -> 0x{new_val:08X}")
        print(f"  (NAND env read will fail CRC; U-Boot falls back to compiled-in defaults)")
        if not args.dry_run:
            patch_nand_offset(data, c['offset'], new_val)

    # Write output
    out_path = args.output or (Path(args.input).stem + '_patched.bin')
    if args.dry_run:
        print(f"\n[dry-run] would write {len(data):,} bytes to {out_path}")
    else:
        Path(out_path).write_bytes(bytes(data))
        print(f"\nWrote {len(data):,} bytes to {out_path}")
        print(f"Place as UBOOT.BIN on SD p1 FAT32 for Stepldr to load.")


if __name__ == '__main__':
    main()
