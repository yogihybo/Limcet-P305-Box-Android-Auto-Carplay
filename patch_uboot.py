#!/usr/bin/env python3
"""
patch_uboot.py - Patch compiled-in environment in ARK1680 U-Boot binary

The U-Boot binary contains a small compiled-in env block used as fallback
when the primary env storage (NAND mtd3) fails its CRC check. This script:
  1. Finds and patches key=value pairs in that block
  2. Patches the ARM32 MOV instructions that load CONFIG_ENV_OFFSET (0x120000)
     to load an invalid address (0xFF000000) instead, forcing CRC failure so
     U-Boot uses the compiled-in defaults

Background:
  The ARK1680 U-Boot encodes CONFIG_ENV_OFFSET=0x120000 as a rotated
  immediate in ARM32 MOV instructions (imm12=0x0812, i.e. 0x12 ROR 16).
  A naive byte-search for \\x00\\x00\\x12\\x00 finds only non-aligned hits
  inside unrelated instructions. The correct approach is to search for the ARM
  instruction encoding at 4-byte-aligned addresses.

  uboot_sdboot.bin is a source-compiled binary (from linux-arkmicro BSP) with
  the sdboot preset already in CONFIG_EXTRA_ENV_SETTINGS. Run --patch-nand-offset
  on it to disable the NAND env so the compiled-in sdboot defaults win.

Examples:
  # Inspect compiled-in env
  python patch_uboot.py -i uboot.bin --dump-env

  # Find ARM MOV instructions that load CONFIG_ENV_OFFSET
  python patch_uboot.py -i uboot.bin --find-nand-offset

  # Source-compiled binary: only needs NAND offset disabled
  python patch_uboot.py -i uboot_sdboot.bin -o uboot_final.bin --patch-nand-offset

  # Original binary: patch compiled-in env AND disable NAND offset
  python patch_uboot.py -i uboot.bin -o uboot_sdboot.bin --mode sdboot --patch-nand-offset

  # Custom root device
  python patch_uboot.py -i uboot.bin -o uboot_sdboot.bin --mode sdboot --root /dev/mmcblk1p2 --patch-nand-offset

  # Manual env patches only (no NAND offset change)
  python patch_uboot.py -i uboot.bin -o uboot_patched.bin --set bootcmd="run sdboot" --set bootdelay=3

  # Dry run — show what would change without writing
  python patch_uboot.py -i uboot.bin --mode sdboot --patch-nand-offset --dry-run
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


def measure_env_capacity(data: bytes, offset: int, max_size: int) -> tuple:
    """
    Measure how many bytes from offset are safe to overwrite.

    Many U-Boot binaries (e.g. a raw NAND dump) don't reserve a fixed-size
    buffer for the compiled-in env — the env strings sit directly against
    whatever binary data follows (command tables, code, ...), with at most a
    few incidental zero bytes of alignment padding. Blindly clearing a fixed
    max_size window in that case overwrites real data. Only source-compiled
    binaries with a real CONFIG_ENV_SIZE buffer (e.g. uboot_sdboot.bin) have
    a large genuine zero-padded region here.

    Returns (original_length, safe_capacity):
      - original_length: bytes from offset through the env block's own
        double-null terminator (i.e. exactly what's already env content).
      - safe_capacity: original_length plus any zero bytes immediately
        following it (verified padding), capped at max_size. It is never
        safe to write more than this many bytes at offset.
    """
    pos = offset
    end = min(offset + max_size, len(data))
    while pos < end:
        null = data.find(b'\x00', pos, end)
        if null == -1:
            pos = end
            break
        if null == pos:
            pos = null + 1
            break
        pos = null + 1
    original_length = pos - offset

    safe_capacity = original_length
    while offset + safe_capacity < end and data[offset + safe_capacity] == 0:
        safe_capacity += 1

    return original_length, safe_capacity


def patch_env_block(data: bytearray, offset: int, patches: dict,
                    max_size: int = 4096) -> bool:
    """
    Merge patches into the existing compiled-in env block and write back.
    Only ever touches bytes verified safe by measure_env_capacity() — never
    the fixed max_size window itself. Returns False if the patched env
    doesn't fit in that safe region.
    """
    existing = parse_env(bytes(data), offset, max_size)
    existing.update(patches)
    serialized = serialize_env(existing)

    original_length, safe_capacity = measure_env_capacity(bytes(data), offset, max_size)

    if len(serialized) > safe_capacity:
        print(f"ERROR: patched env ({len(serialized)} B) exceeds the {safe_capacity} B "
              f"safe to rewrite here")
        print(f"       ({original_length} B of existing env + "
              f"{safe_capacity - original_length} B of verified zero padding).")
        print("       Writing more would overwrite non-zero data immediately after the")
        print("       env block — expected for a raw/dumped uboot.bin with no reserved")
        print("       env buffer. Use a source-compiled binary with a real env buffer")
        print("       (e.g. uboot_sdboot.bin) instead, or reduce the env entries.")
        return False

    write_len = max(len(serialized), original_length)
    padded = serialized + b'\x00' * (write_len - len(serialized))
    data[offset:offset + write_len] = padded
    return True


# ---------------------------------------------------------------------------
# NAND env offset — ARM instruction patching
# ---------------------------------------------------------------------------

# NAND env partition start on Prado: mtd3 (U-Boot-Env) at 128K+512K+512K = 0x120000
DEFAULT_NAND_ENV_OFFSET = 0x00120000

# Replacement offset — beyond any real NAND range, guarantees CRC failure
INVALID_NAND_OFFSET = 0xFF000000

# ARM32 rotated-immediate encoding of 0x120000:
#   0x12 ROR 16  → imm8=0x12, rot4=8 → imm12 = (8<<8)|0x12 = 0x0812
_IMM12_VALID   = 0x0812

# ARM32 rotated-immediate encoding of 0xFF000000:
#   0xFF ROR 8   → imm8=0xFF, rot4=4 → imm12 = (4<<8)|0xFF = 0x04FF
_IMM12_INVALID = 0x04FF

# Instruction mask/pattern for "AL-condition MOV Rd, #0x120000" (any Rd):
#   bits 31-28 = 1110 (AL), 27-26 = 00, 25 (I) = 1, 24-21 = 1101 (MOV),
#   20 (S) = 0, 19-16 = 0000 (Rn ignored), 15-12 = Rd (variable), 11-0 = imm12
_MOV_MASK    = 0xFFFF0FFF   # mask out Rd nibble (bits 15-12)
_MOV_PATTERN = 0xE3A00000 | _IMM12_VALID   # 0xE3A00812 with Rd=0


def find_nand_offset_candidates(data: bytes) -> list:
    """
    Find all 4-byte-aligned ARM32 MOV instructions that load DEFAULT_NAND_ENV_OFFSET.
    Returns list of dicts with 'offset', 'word', and 'rd' keys.
    """
    candidates = []
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from('<I', data, off)[0]
        if (w & _MOV_MASK) == _MOV_PATTERN:
            rd = (w >> 12) & 0xF
            candidates.append({'offset': off, 'word': w, 'rd': rd})
    return candidates


def patch_nand_offset(data: bytearray, candidates: list,
                      new_imm12: int = _IMM12_INVALID) -> int:
    """
    Patch all found MOV Rd, #0x120000 instructions to MOV Rd, #0xFF000000.
    Returns the number of instructions patched.
    """
    count = 0
    for c in candidates:
        old_w = struct.unpack_from('<I', data, c['offset'])[0]
        new_w = (old_w & ~0xFFF) | new_imm12
        struct.pack_into('<I', data, c['offset'], new_w)
        count += 1
    return count


# ---------------------------------------------------------------------------
# Preset profiles
# ---------------------------------------------------------------------------

PRESETS = {
    'sdboot': {
        'description': 'Boot from SD card — ext4 rootfs on /dev/mmcblk0p2',
        'env': {
            'bootcmd':    'run sdboot',
            'bootfile':   'zImage',
            'mmcdev':     '1',
            'bootdelay':  '3',
            'sdboot':     (
                'run sdbootargs; '
                'fatload mmc ${mmcdev}:1 ${loadaddr} ${bootfile}; '
                'bootz ${loadaddr}'
            ),
            'sdbootargs': (
                'setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M '
                'root=/dev/mmcblk0p2 rootfstype=ext4 rootwait rw'
            ),
            # usbboot convenience: 'run usbboot' at U-Boot prompt to boot from USB drive
            # USB drive layout: FAT p1 with zImage, ext4 p2 as rootfs (/dev/sda2 in kernel)
            'usbboot':     (
                'usb start; '
                'fatload usb 0:1 ${loadaddr} ${bootfile}; '
                'run usbbootargs; '
                'bootz ${loadaddr}'
            ),
            'usbbootargs': (
                'setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M '
                'root=/dev/sda2 rootfstype=ext4 rootwait rw'
            ),
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
                    help='List ARM MOV instructions that load CONFIG_ENV_OFFSET and exit')
    ap.add_argument('--mode', choices=list(PRESETS.keys()),
                    help='Apply a preset patch profile to the compiled-in env')
    ap.add_argument('--root', default='/dev/mmcblk0p2', metavar='DEVICE',
                    help='Root device for sdboot mode (default: /dev/mmcblk0p2)')
    ap.add_argument('--set', metavar='KEY=VALUE', action='append', dest='setenv',
                    help='Set a compiled-in env variable (repeatable, applied after --mode)')
    ap.add_argument('--patch-nand-offset', action='store_true',
                    help=(
                        'Patch all ARM MOV #0x120000 instructions to MOV #0xFF000000, '
                        'forcing NAND env CRC failure so compiled-in defaults are used'
                    ))
    ap.add_argument('--env-block-size', type=int, default=4096, metavar='BYTES',
                    help='Max bytes after the env offset to scan for parsing and safe '
                         'padding (default: 4096) — not a guarantee that many bytes '
                         'are writable; see measure_env_capacity()')
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

    # Always find candidates (used for --find-nand-offset and --patch-nand-offset)
    candidates = find_nand_offset_candidates(data_ro)

    # --find-nand-offset
    if args.find_nand_offset:
        if not candidates:
            print(f"\nNo ARM MOV #0x{DEFAULT_NAND_ENV_OFFSET:08X} instructions found.")
            print("  (imm12=0x0812 pattern at 4-byte-aligned addresses)")
        else:
            print(f"\nARM MOV #0x{DEFAULT_NAND_ENV_OFFSET:08X} candidates "
                  f"(imm12=0x{_IMM12_VALID:03X}):")
            for c in candidates:
                print(f"  {c['offset']:#010x}: {c['word']:#010x}  "
                      f"MOV r{c['rd']}, #0x{DEFAULT_NAND_ENV_OFFSET:08X}")
            print(f"\nRun --patch-nand-offset to redirect all to #0x{INVALID_NAND_OFFSET:08X}.")
        return

    # Collect env patches
    patches = {}

    if args.mode:
        preset = PRESETS[args.mode]
        print(f"\nMode: {args.mode} — {preset['description']}")
        patches.update(preset['env'])
        if args.mode == 'sdboot' and args.root != '/dev/mmcblk0p2':
            patches['sdbootargs'] = (
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

    if not patches and not args.patch_nand_offset:
        print("\nNothing to patch. Use --mode, --set, or --patch-nand-offset.")
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
    if args.patch_nand_offset:
        if not candidates:
            print(f"\nWARNING: no ARM MOV #0x{DEFAULT_NAND_ENV_OFFSET:08X} instructions found.")
            print("  NAND env offset was NOT patched.")
        else:
            print(f"\nNAND offset patch — redirecting "
                  f"#0x{DEFAULT_NAND_ENV_OFFSET:08X} → #0x{INVALID_NAND_OFFSET:08X}:")
            for c in candidates:
                new_w = (c['word'] & ~0xFFF) | _IMM12_INVALID
                print(f"  {c['offset']:#010x}: {c['word']:#010x} → {new_w:#010x}  "
                      f"(MOV r{c['rd']}, #0x{DEFAULT_NAND_ENV_OFFSET:08X} "
                      f"→ MOV r{c['rd']}, #0x{INVALID_NAND_OFFSET:08X})")
            print("  NAND env CRC will fail; U-Boot falls back to compiled-in defaults.")
            if not args.dry_run:
                patch_nand_offset(data, candidates)

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
