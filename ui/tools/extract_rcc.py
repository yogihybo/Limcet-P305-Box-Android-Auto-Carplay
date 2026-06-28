#!/usr/bin/env python3
"""
extract_rcc.py - Extract Qt compiled resource files (.rcc)

Qt RCC v1 binary format:
  0x00  4B  magic  'qres'
  0x04  4B  version (big-endian, 1)
  0x08  4B  tree_offset   (from file start)
  0x0C  4B  data_offset   (from file start)
  0x10  4B  names_offset  (from file start)

Tree node (14 bytes each):
  0  4B  name_offset  (into names section)
  4  2B  flags        (0=dir, 1=file, 8=compressed)
  6  4B  child_count (dir) OR locale (file)
  10 4B  child_offset (dir, index of first child) OR data_offset (file, into data section)

Names section:
  2B  name_length (chars)
  4B  hash
  nB  UTF-16BE name

Data section (per file):
  4B  data_length
  nB  data bytes (possibly zlib-compressed if flag bit 8)

Usage:
  python extract_rcc.py Setting.rcc
  python extract_rcc.py Setting.rcc -o out_dir
  python extract_rcc.py resources/*.rcc -o extracted_ui
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_be32(data, off):
    return struct.unpack_from('>I', data, off)[0]


def read_be16(data, off):
    return struct.unpack_from('>H', data, off)[0]


def read_name(data, names_off, name_node_off):
    off = names_off + name_node_off
    length = read_be16(data, off)
    # skip hash (4 bytes)
    chars = data[off + 6: off + 6 + length * 2]
    return chars.decode('utf-16-be')


def walk_tree(data, tree_off, data_off, names_off, node_idx, path):
    """Yield (virtual_path, raw_bytes) for every file in the tree."""
    off = tree_off + node_idx * 14
    name_off = read_be32(data, off)
    flags = read_be16(data, off + 4)

    name = read_name(data, names_off, name_off) if node_idx != 0 else ''
    cur_path = path + '/' + name if name else path

    is_dir = bool(flags & 0x02)
    is_compressed = bool(flags & 0x01)

    if is_dir:
        child_count = read_be32(data, off + 6)
        first_child = read_be32(data, off + 10)
        for i in range(child_count):
            yield from walk_tree(data, tree_off, data_off, names_off,
                                  first_child + i, cur_path)
    else:
        # file
        file_data_off = data_off + read_be32(data, off + 10)
        length = read_be32(data, file_data_off)
        raw = data[file_data_off + 4: file_data_off + 4 + length]
        if is_compressed:
            # strip Qt's 4-byte uncompressed size prefix
            raw = zlib.decompress(raw[4:])
        yield (cur_path.lstrip('/'), raw)


def extract_rcc(rcc_path: Path, out_dir: Path, verbose: bool):
    data = rcc_path.read_bytes()

    if data[:4] != b'qres':
        print(f"  ERROR: not a Qt RCC file (magic={data[:4]!r})")
        return 0

    version = read_be32(data, 4)
    if version != 1:
        print(f"  ERROR: unsupported RCC version {version} (expected 1)")
        return 0

    tree_off  = read_be32(data, 8)
    data_off  = read_be32(data, 12)
    names_off = read_be32(data, 16)

    if verbose:
        print(f"  tree=0x{tree_off:X} data=0x{data_off:X} names=0x{names_off:X}")

    count = 0
    for vpath, raw in walk_tree(data, tree_off, data_off, names_off, 0, ''):
        dest = out_dir / rcc_path.stem / vpath
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(raw)
        if verbose:
            print(f"  {vpath}  ({len(raw):,} B)")
        count += 1

    return count


def main():
    ap = argparse.ArgumentParser(description='Extract Qt .rcc resource files')
    ap.add_argument('rcc', nargs='+', help='One or more .rcc files (wildcards ok)')
    ap.add_argument('-o', '--out', default='rcc_extracted',
                    help='Output directory (default: rcc_extracted/)')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='Print each extracted file')
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    for pattern in args.rcc:
        paths = list(Path('.').glob(pattern)) or [Path(pattern)]
        for p in sorted(paths):
            print(f"\n{p.name}:")
            n = extract_rcc(p, out_dir, args.verbose)
            print(f"  extracted {n} files -> {out_dir / p.stem}/")
            total += n

    print(f"\nTotal: {total} files extracted to {out_dir}/")


if __name__ == '__main__':
    main()
