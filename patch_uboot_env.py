#!/usr/bin/env python3
"""
patch_uboot_env.py - RELOCATE the compiled-in default environment in an
ARK1680 U-Boot binary so a full-size boot command fits entirely in the binary.

Why this exists (vs. patch_uboot.py)
------------------------------------
patch_uboot.py can only *edit* the compiled-in env in place. On a raw NAND
dump / Holden-derived uboot.bin that env is ~73 bytes packed hard against real
data (a pointer table), so a real SD-boot command (bootargs + fatload + bootz,
~150 B) will never fit. See docs/UBOOT_SDBOOT_INVESTIGATION.md §1-§4.

The default env is NOT copied by value into the binary at a fixed buffer; it is
referenced by *pointer*. `set_default_env()` / `env_relocate()` call
`himport_r(&env_htab, default_environment, sizeof(default_environment), ...)`.
So instead of growing the env in place we:

  1. Write the new (arbitrarily long) env into a genuinely-free zero region
     that is still inside the loaded image (below __bss_start, so it is copied
     and relocated with everything else).
  2. Repoint every absolute-literal reference to `default_environment` at the
     new region.
  3. Bump every `sizeof(default_environment)` immediate that bounds himport, so
     himport reads the whole new env instead of truncating at the old 73 bytes.
  4. (optional) apply the same NAND-env-offset corruption as patch_uboot.py so
     the on-NAND env fails its CRC and this relocated default is the one used.

This is a data relocation + two constant patches. No ARM code is injected, so
it avoids the machine-code / calling-convention brick risk that
docs/UBOOT_SDBOOT_INVESTIGATION.md §4 warned about. Everything below is found
by byte-scan (link base derived from a pointer histogram), no disassembler
required.

Offsets on the live Prado dump (Prado firmware dump/mtd1-mtd2_uboot/extracted/
uboot.bin), for reference - the script re-derives them, it does not hardcode:
  CONFIG_SYS_TEXT_BASE = 0x30000
  default_environment  = file 0x41f77 (va 0x71f77)
  pointer literals     = 0xb948 0xb988 0xba38 0xba78 0xbba8  (each = 0x00071f77)
  himport size (0x49)  = 0xb9d8 0xba54  (mov r2,#0x49)
  __bss_start ofs      = 0x54ef8   (free region must end at or below this)

Examples
--------
  # Inspect: base, env, all pointer/size sites, and free regions
  python patch_uboot_env.py -i uboot.bin --analyze

  # Relocate a full SD-boot env + corrupt NAND offset (recommended)
  python patch_uboot_env.py -i uboot.bin -o uboot_relocenv.bin \
      --preset sdboot --patch-nand-offset

  # Custom root device
  python patch_uboot_env.py -i uboot.bin -o uboot_relocenv.bin \
      --preset sdboot --root /dev/mmcblk1p2 --patch-nand-offset

  # Fully custom env, keeping the original backlight/screen keys
  python patch_uboot_env.py -i uboot.bin -o out.bin --keep-original \
      --set 'bootcmd=run sdboot' \
      --set 'sdboot=fatload mmc 0:1 1000000 zImage;bootz 1000000' \
      --set 'bootdelay=2' --patch-nand-offset

  # Pin the free region explicitly instead of auto-picking the largest
  python patch_uboot_env.py -i uboot.bin -o out.bin --preset sdboot \
      --region-offset 0x50fdd --patch-nand-offset

  # Dry run
  python patch_uboot_env.py -i uboot.bin --preset sdboot --patch-nand-offset --dry-run
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path


# ===========================================================================
# Link base + default_environment discovery
# ===========================================================================

def _aligned_words(data):
    for o in range(0, len(data) - 3, 4):
        yield o, struct.unpack_from('<I', data, o)[0]


def find_env_block(data):
    """
    Locate the compiled-in default_environment array.

    Returns (start, end) file offsets: start is the first byte of the first
    key=value entry, end is one past the terminating double-NUL. The env is a
    run of printable, '='-containing, NUL-separated entries; it is bounded on
    both sides by non-env binary data (padding / pointer tables), which is how
    we find its true extent regardless of what the first key happens to be.
    """
    marker = b'bootcmd='
    pos = 0
    anchor = -1
    while True:
        idx = data.find(marker, pos)
        if idx == -1:
            break
        if idx == 0 or data[idx - 1] == 0:
            anchor = idx
            break
        pos = idx + 1
    if anchor == -1:
        return None

    def printable_entry(b):
        return b != b'' and all(32 <= c < 127 for c in b)

    # walk back over well-formed entries
    start = anchor
    p = anchor
    while p > 0:
        e = p - 1                       # NUL terminating the previous entry
        s = e
        while s > 0 and data[s - 1] != 0:
            s -= 1
        entry = data[s:e]
        if printable_entry(entry) and b'=' in entry:
            start = s
            p = s
        else:
            break

    # walk forward to the terminating double-NUL
    q = anchor
    end = anchor
    while q < len(data):
        e = data.find(b'\x00', q)
        if e == -1:
            end = len(data)
            break
        entry = data[q:e]
        if entry == b'':                # empty entry = the terminator
            end = e + 1
            break
        if not printable_entry(entry):
            end = q
            break
        q = e + 1
    return start, end


def derive_env_pointer(data, base, env_start):
    """
    Find the runtime pointer *value* that references default_environment.

    The image uses absolute pointers (fixed up at runtime via .rel.dyn). The
    literal that points at the env symbol has value `base + env_symbol_off`,
    where env_symbol_off is at or a little before the first detected entry, and
    it recurs at several code sites. Search aligned words for the value that
    lands just inside/before the env block (VA space) and appears >= 2 times.
    """
    lo = base + env_start - 0x20
    hi = base + env_start + 0x8
    cand = Counter(w for _, w in _aligned_words(data) if lo <= w < hi)
    if not cand:
        return None
    ptr_val, count = cand.most_common(1)[0]
    if count < 2:
        return None
    return ptr_val


def read_text_base(data):
    """
    CONFIG_SYS_TEXT_BASE from the U-Boot header word at file 0x40. It is a
    round, low-ish load address; return None if it doesn't look like one.
    """
    text_base = struct.unpack_from('<I', data, 0x40)[0]
    if 0 < text_base < 0x10000000 and (text_base & 0xFFFF) == 0:
        return text_base
    return None


# ===========================================================================
# Pointer-literal and size-immediate sites
# ===========================================================================

def find_pointer_literals(data, ptr_val):
    tgt = struct.pack('<I', ptr_val)
    return [o for o in range(0, len(data) - 3, 4)
            if data[o:o + 4] == tgt]


def find_size_sites(data, ptr_literals, size_val, window=0x80):
    """
    Find `mov r2, #size_val` (himport's size argument) instructions that sit
    near a default_environment pointer literal - this distinguishes the real
    himport call sites from unrelated `mov r2,#imm` elsewhere. The window must
    be generous: a call site can load its env pointer from a literal pool up to
    ~0x60 bytes away (e.g. set_default_env at 0xB9D8 uses the pool at 0xBA38).

    ARM: mov r2,#imm8  ->  0xE3A02000 | imm8  (imm8 == size_val, size_val<256).
    """
    if size_val >= 0x100:
        # only imm8-encoded sizes are auto-detected; the ark build uses 0x49
        pass
    target = 0xE3A02000 | (size_val & 0xFFF)
    sites = []
    for o, w in _aligned_words(data):
        if w == target:
            if any(abs(o - lit) <= window for lit in ptr_literals):
                sites.append(o)
    return sites


# ===========================================================================
# Free region discovery
# ===========================================================================

def bss_start_ofs(data):
    """
    __bss_start offset from the U-Boot header table just past the vectors.
    Data written at or above this is BSS (zeroed at runtime) and would be lost;
    the relocated env must live below it. Falls back to file length if the
    header word looks implausible.
    """
    w = struct.unpack_from('<I', data, 0x44)[0]
    if 0 < w <= len(data):
        return w
    return len(data)


def find_zero_regions(data, min_len, limit):
    """All maximal runs of >= min_len zero bytes that END at or below `limit`."""
    runs = []
    n = min(len(data), limit)
    j = 0
    while j < n:
        if data[j] == 0:
            k = j
            while k < n and data[k] == 0:
                k += 1
            if k - j >= min_len:
                runs.append((j, k - j))
            j = k
        else:
            j += 1
    return runs


# ===========================================================================
# ARM rotated-immediate encoding (for the size patch)
# ===========================================================================

def encode_arm_imm(value):
    """Return (imm12) for a MOV of `value`, or None if not representable."""
    for rot in range(16):
        v = ((value << (2 * rot)) | (value >> (32 - 2 * rot))) & 0xFFFFFFFF
        if v <= 0xFF:
            return (rot << 8) | v
    return None


def choose_size(env_len, region_len):
    """
    Pick the smallest single-MOV-encodable size S with env_len <= S <= region_len.
    himport reads exactly S bytes; S >= env_len imports all our vars, and
    S <= region_len keeps it inside the zero run (trailing zeros parse as empty
    entries and are skipped), never spilling into real data after the region.
    """
    for s in range(env_len, region_len + 1):
        if encode_arm_imm(s) is not None:
            return s
    return None


# ===========================================================================
# NAND env offset corruption (same technique as patch_uboot.py)
# ===========================================================================

DEFAULT_NAND_ENV_OFFSET = 0x00120000
INVALID_NAND_OFFSET = 0xFF000000
_IMM12_VALID = 0x0812        # 0x120000 = 0x12 ROR 16
_IMM12_INVALID = 0x04FF      # 0xFF000000 = 0xFF ROR 8
_MOV_MASK = 0xFFFF0FFF
_MOV_PATTERN = 0xE3A00000 | _IMM12_VALID


def find_nand_offset_candidates(data):
    out = []
    for o, w in _aligned_words(data):
        if (w & _MOV_MASK) == _MOV_PATTERN:
            out.append((o, w, (w >> 12) & 0xF))
    return out


def patch_nand_offset(data, candidates):
    for off, w, _rd in candidates:
        new_w = (w & ~0xFFF) | _IMM12_INVALID
        struct.pack_into('<I', data, off, new_w)
    return len(candidates)


# ===========================================================================
# Env (de)serialisation
# ===========================================================================

def parse_env(data, start, end):
    env = {}
    for entry in data[start:end].split(b'\x00'):
        if not entry:
            continue
        s = entry.decode('latin1')
        if '=' in s:
            k, v = s.split('=', 1)
            env[k] = v
    return env


def serialize_env(env):
    return b'\x00'.join(f'{k}={v}'.encode('latin1') for k, v in env.items()) + b'\x00\x00'


# ===========================================================================
# Presets
# ===========================================================================

def preset_sdboot(root):
    return {
        'bootcmd':    'run sdboot',
        'bootdelay':  '2',
        'bootfile':   'zImage',
        'loadaddr':   '0x1000000',
        'sdboot':     ('setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M '
                       f'root={root} rootfstype=ext4 rootwait rw;'
                       'fatload mmc 0:1 ${loadaddr} ${bootfile};'
                       'bootz ${loadaddr}'),
        # convenience at the prompt: `run usbboot`
        'usbboot':    ('usb start;'
                       'setenv bootargs console=ttyS0,115200n8 console=tty0 mem=180M '
                       'root=/dev/sda2 rootfstype=ext4 rootwait rw;'
                       'fatload usb 0:1 ${loadaddr} ${bootfile};'
                       'bootz ${loadaddr}'),
        # keep a way back to the stock behaviour
        'nandboot':   'run nandbootcmd',
    }


# ===========================================================================
# Discovery bundle
# ===========================================================================

class UBootEnvImage:
    def __init__(self, data):
        self.data = bytearray(data)
        blk = find_env_block(data)
        if blk is None:
            raise ValueError("compiled-in env block not found (no null-preceded 'bootcmd=')")
        self.env_start, self.env_end = blk
        self.env = parse_env(data, self.env_start, self.env_end)

        self.base = read_text_base(data)
        if self.base is None:
            raise ValueError("could not read header CONFIG_SYS_TEXT_BASE at file 0x40")
        self.env_ptr_val = derive_env_pointer(data, self.base, self.env_start)
        if self.env_ptr_val is None:
            raise ValueError("could not find default_environment pointer "
                             "(no repeated aligned word into env block)")
        # The pointer gives the true symbol start, which can be a few bytes
        # before the entry walk-back reached (the first key may be preceded by
        # non-NUL padding rather than a clean separator). Trust the pointer and
        # re-parse from there so no leading key (e.g. backlight) is dropped.
        sym_off = self.env_ptr_val - self.base
        if 0 <= sym_off <= self.env_start:
            self.env_start = sym_off
            self.env = parse_env(data, self.env_start, self.env_end)
        self.env_sym_off = self.env_ptr_val - self.base
        if not (0 <= self.env_sym_off < len(data)):
            raise ValueError(f"derived env symbol offset 0x{self.env_sym_off:x} out of range")
        self.env_len = self.env_end - self.env_start

        self.ptr_literals = find_pointer_literals(data, self.env_ptr_val)
        # sizeof(default_environment): from the symbol start to its double-NUL.
        # Detect the immediate actually used near the pointer literals.
        self.detected_sizeof = self._detect_sizeof(data)
        self.size_sites = find_size_sites(data, self.ptr_literals, self.detected_sizeof)
        self.bss_ofs = bss_start_ofs(data)
        self.nand_candidates = find_nand_offset_candidates(data)

    def _detect_sizeof(self, data):
        """
        Find the mov r2,#imm value used as himport's size near a pointer
        literal. Try the true content length and a small neighbourhood (the
        compiler's sizeof may include padding beyond the visible double-NUL).
        """
        content_len = self.env_end - self.env_sym_off  # from symbol start to end
        for delta in (0, 1, 2, 3, -1, -2, 4, 5):
            cand = content_len + delta
            if 0 < cand < 0x100:
                target = 0xE3A02000 | cand
                for o, w in _aligned_words(data):
                    if w == target and any(abs(o - lit) <= 0x80 for lit in self.ptr_literals):
                        return cand
        # fallback: visible content length
        return content_len & 0xFF


# ===========================================================================
# Reporting
# ===========================================================================

def report(img):
    d = img.data
    print(f"CONFIG_SYS_TEXT_BASE  : 0x{img.base:08X}")
    print(f"default_environment   : file 0x{img.env_sym_off:06X}  va 0x{img.env_ptr_val:08X}")
    print(f"  detected env entries: file 0x{img.env_start:06X}..0x{img.env_end:06X} "
          f"({img.env_len} B visible)")
    for k, v in img.env.items():
        print(f"      {k}={v}")
    print(f"  sizeof used by himport: 0x{img.detected_sizeof:02X} ({img.detected_sizeof} B)")
    print(f"__bss_start offset     : 0x{img.bss_ofs:06X} (relocated env must end at/below this)")
    print(f"\npointer literals (-> 0x{img.env_ptr_val:08X}), {len(img.ptr_literals)} found:")
    for o in img.ptr_literals:
        print(f"    file 0x{o:06X}")
    print(f"himport size immediates (mov r2,#0x{img.detected_sizeof:02X}), "
          f"{len(img.size_sites)} found:")
    for o in img.size_sites:
        print(f"    file 0x{o:06X}: {struct.unpack_from('<I', d, o)[0]:08X}")
    print(f"NAND env-offset MOVs (mov rX,#0x{DEFAULT_NAND_ENV_OFFSET:08X}), "
          f"{len(img.nand_candidates)} found:")
    for o, w, rd in img.nand_candidates:
        print(f"    file 0x{o:06X}: {w:08X}  mov r{rd},#0x{DEFAULT_NAND_ENV_OFFSET:08X}")
    regions = find_zero_regions(bytes(d), 64, img.bss_ofs)
    regions.sort(key=lambda r: -r[1])
    print(f"\nfree zero regions below __bss_start (top 12 by size):")
    for off, ln in regions[:12]:
        print(f"    file 0x{off:06X}  {ln} B  (va 0x{img.base + off:08X})")


# ===========================================================================
# The relocation patch
# ===========================================================================

def relocate(img, new_env, region_offset, dry_run):
    d = img.data
    serialized = serialize_env(new_env)
    env_len = len(serialized)

    # sanity on discovery
    if not img.ptr_literals:
        print("ERROR: no default_environment pointer literals found; refusing.")
        return False
    if not img.size_sites:
        print("ERROR: no himport size immediates found; refusing (would truncate env).")
        return False

    # choose region
    regions = find_zero_regions(bytes(d), env_len, img.bss_ofs)
    if region_offset is None:
        if not regions:
            print(f"ERROR: no zero region >= {env_len} B below __bss_start "
                  f"(0x{img.bss_ofs:06X}). Shrink the env.")
            return False
        regions.sort(key=lambda r: -r[1])
        region_offset, region_len = regions[0]
    else:
        # validate the user-picked region
        region_len = 0
        n = min(len(d), img.bss_ofs)
        while region_offset + region_len < n and d[region_offset + region_len] == 0:
            region_len += 1
        if region_offset + region_len > img.bss_ofs:
            region_len = img.bss_ofs - region_offset
        if region_len < env_len:
            print(f"ERROR: region 0x{region_offset:06X} holds only {region_len} zero B, "
                  f"need {env_len}.")
            return False
        if region_offset >= img.bss_ofs:
            print(f"ERROR: region 0x{region_offset:06X} is at/above __bss_start "
                  f"(0x{img.bss_ofs:06X}); would be zeroed at runtime.")
            return False

    # verify region is genuinely all-zero
    if any(b != 0 for b in d[region_offset:region_offset + region_len]):
        print(f"ERROR: region 0x{region_offset:06X} is not all-zero; refusing.")
        return False

    # pick himport size
    size = choose_size(env_len, region_len)
    if size is None:
        print(f"ERROR: no ARM-encodable himport size in [{env_len}, {region_len}]; "
              f"shrink the env or pick a larger region.")
        return False
    size_imm12 = encode_arm_imm(size)

    new_ptr_val = img.base + region_offset

    print(f"\nRelocating default_environment:")
    print(f"  new region     : file 0x{region_offset:06X} (va 0x{new_ptr_val:08X}), "
          f"{region_len} zero B")
    print(f"  env size       : {env_len} B  ->  {len(new_env)} keys")
    for k, v in new_env.items():
        print(f"      {k}={v}")
    print(f"  pointer patches: 0x{img.env_ptr_val:08X} -> 0x{new_ptr_val:08X} at "
          + " ".join(f"0x{o:06X}" for o in img.ptr_literals))
    print(f"  size patches   : mov r2,#0x{img.detected_sizeof:02X} -> mov r2,#0x{size:02X} at "
          + " ".join(f"0x{o:06X}" for o in img.size_sites))

    if dry_run:
        return True

    # 1. write the env into the free region
    d[region_offset:region_offset + env_len] = serialized
    # 2. repoint every pointer literal
    for o in img.ptr_literals:
        struct.pack_into('<I', d, o, new_ptr_val)
    # 3. bump every himport size immediate
    new_mov = 0xE3A02000 | size_imm12
    for o in img.size_sites:
        struct.pack_into('<I', d, o, new_mov)
    return True


# ===========================================================================
# CLI
# ===========================================================================

def main():
    ap = argparse.ArgumentParser(
        description='Relocate + grow the compiled-in default env in an ARK1680 U-Boot binary',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument('-i', '--input', required=True, help='Input U-Boot binary')
    ap.add_argument('-o', '--output', help='Output patched binary '
                    '(default: <input>_relocenv.bin)')
    ap.add_argument('--analyze', action='store_true',
                    help='Report base, env, pointer/size sites, free regions; then exit')
    ap.add_argument('--preset', choices=['sdboot'],
                    help='Start from a preset env (sdboot: full SD-card boot)')
    ap.add_argument('--root', default='/dev/mmcblk0p2', metavar='DEVICE',
                    help='Root device for the sdboot preset (default: /dev/mmcblk0p2)')
    ap.add_argument('--set', metavar='KEY=VALUE', action='append', dest='setenv',
                    help='Set/override an env key (repeatable, applied after --preset)')
    ap.add_argument('--keep-original', action='store_true',
                    help='Merge the binary\'s existing compiled-in keys under the new env '
                         '(preset/--set win on conflict)')
    ap.add_argument('--region-offset', type=lambda s: int(s, 0), metavar='OFF',
                    help='Force the free-region file offset (default: largest zero run)')
    ap.add_argument('--patch-nand-offset', action='store_true',
                    help='Also corrupt the NAND env offset MOVs so the on-NAND env fails '
                         'CRC and this relocated default env is the one used')
    ap.add_argument('--dry-run', action='store_true',
                    help='Show all changes without writing output')
    args = ap.parse_args()

    data = Path(args.input).read_bytes()
    try:
        img = UBootEnvImage(data)
    except ValueError as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    print(f"Input: {args.input} ({len(data):,} bytes)")
    report(img)

    if args.analyze:
        return

    # build the new env
    new_env = {}
    if args.keep_original:
        new_env.update(img.env)
    if args.preset == 'sdboot':
        new_env.update(preset_sdboot(args.root))
    if args.setenv:
        for item in args.setenv:
            if '=' not in item:
                print(f"ERROR: --set needs KEY=VALUE, got {item!r}")
                sys.exit(1)
            k, v = item.split('=', 1)
            new_env[k] = v

    if not new_env and not args.patch_nand_offset:
        print("\nNothing to do. Use --preset, --set, and/or --patch-nand-offset "
              "(or --analyze).")
        sys.exit(1)

    ok = True
    if new_env:
        ok = relocate(img, new_env, args.region_offset, args.dry_run)
        if not ok:
            sys.exit(1)

    if args.patch_nand_offset:
        if not img.nand_candidates:
            print("\nWARNING: no NAND env-offset MOVs found; NOT patched.")
        else:
            print(f"\nNAND offset patch: 0x{DEFAULT_NAND_ENV_OFFSET:08X} -> "
                  f"0x{INVALID_NAND_OFFSET:08X} at "
                  + " ".join(f"0x{o:06X}" for o, _w, _rd in img.nand_candidates))
            if not args.dry_run:
                patch_nand_offset(img.data, img.nand_candidates)

    out = args.output or (Path(args.input).stem + '_relocenv.bin')
    if args.dry_run:
        print(f"\n[dry-run] would write {len(img.data):,} bytes to {out}")
    else:
        Path(out).write_bytes(bytes(img.data))
        print(f"\nWrote {len(img.data):,} bytes to {out}")
        print("Place as UBOOT.BIN on SD p1 (FAT32) for Stepldr to load.")


if __name__ == '__main__':
    main()
