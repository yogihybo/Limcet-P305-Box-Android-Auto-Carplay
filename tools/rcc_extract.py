#!/usr/bin/env python3
"""Extract a Qt binary resource bundle (.rcc, the 'qres' format) to a directory.

The head unit's UI skins live in `msnprofile/resources/*.rcc` — Qt binary
resource bundles (`rcc -binary` output) full of PNG sprites. Qt ships a packer
(`rcc`) but no unpacker, so this reproduces the read side of the format
(qresource.cpp): it walks the resource tree and writes every embedded file out,
decompressing zlib/zstd entries.

Supports qres format versions 1-3 (this firmware uses v1).

Usage:
    python rcc_extract.py <input.rcc> <output-dir>
    python rcc_extract.py --list <input.rcc>        # list contents, extract nothing

Repacking after editing a sprite (needs a Qt `rcc`; keep filenames + sizes):
    rcc -binary Setting.qrc -o Setting.rcc          # Qt4 rcc -> version-1 qres
    # a Qt5 rcc works too: rcc --binary --format-version 1 Setting.qrc -o Setting.rcc
A matching <name>.qrc listing every extracted path is written alongside the
output so it can be fed straight back to rcc.
"""
import sys, os, struct, zlib, argparse

FLAG_ZLIB = 0x01
FLAG_DIR  = 0x02
FLAG_ZSTD = 0x04


def _u32(b, o): return struct.unpack_from(">I", b, o)[0]
def _u16(b, o): return struct.unpack_from(">H", b, o)[0]


def extract(path, outdir=None):
    """Parse a .rcc file. If outdir is None, list only (write nothing).

    Returns a list of (relpath, uncompressed_size, was_compressed).
    """
    d = open(path, "rb").read()
    if d[:4] != b"qres":
        raise ValueError("not a qres (.rcc) file: bad magic")
    version = _u32(d, 4)
    tree_off, data_off, name_off = _u32(d, 8), _u32(d, 12), _u32(d, 16)
    node_sz = 22 if version >= 3 else 14   # v3 adds an 8-byte last-modified field

    def name_at(off):
        n = _u16(d, name_off + off)                       # length in UTF-16 chars
        return d[name_off + off + 6: name_off + off + 6 + n * 2].decode("utf-16-be")

    def data_at(off):
        p = data_off + off
        return d[p + 4: p + 4 + _u32(d, p)]               # 4-byte length prefix

    files = []

    def walk(idx, prefix):
        base = tree_off + idx * node_sz
        flags = _u16(d, base + 4)
        nm = "" if idx == 0 else name_at(_u32(d, base))
        if flags & FLAG_DIR:
            count, child0 = _u32(d, base + 6), _u32(d, base + 10)
            here = prefix if idx == 0 else prefix + "/" + nm
            for c in range(child0, child0 + count):
                walk(c, here)
        else:
            raw = data_at(_u32(d, base + 10))
            comp = bool(flags & (FLAG_ZLIB | FLAG_ZSTD))
            if flags & FLAG_ZSTD:
                import zstandard
                raw = zstandard.ZstdDecompressor().decompress(raw[4:])
            elif flags & FLAG_ZLIB:
                raw = zlib.decompress(raw[4:])            # skip 4-byte size prefix
            rel = (prefix + "/" + nm).lstrip("/")
            files.append((rel, len(raw), comp))
            if outdir is not None:
                dst = os.path.join(outdir, rel.replace("/", os.sep))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, "wb").write(raw)

    walk(0, "")
    files.sort()
    return files, version


def _write_qrc(files, outdir, stem):
    qrc = os.path.join(outdir, stem + ".qrc")
    with open(qrc, "w", encoding="utf-8") as f:
        f.write('<!DOCTYPE RCC><RCC version="1.0">\n<qresource>\n')
        for rel, _, _ in files:
            f.write(f"    <file>{rel}</file>\n")
        f.write("</qresource>\n</RCC>\n")
    return qrc


def main():
    ap = argparse.ArgumentParser(description="Extract a Qt .rcc (qres) bundle.")
    ap.add_argument("input")
    ap.add_argument("outdir", nargs="?", help="output dir (omit with --list)")
    ap.add_argument("--list", action="store_true", help="list contents only")
    args = ap.parse_args()

    if args.list:
        files, version = extract(args.input, None)
    else:
        if not args.outdir:
            ap.error("outdir is required unless --list is given")
        files, version = extract(args.input, args.outdir)

    print(f"qres v{version}: {len(files)} files")
    for rel, sz, comp in files:
        print(f"  {sz:>9}  {'z' if comp else ' '}  {rel}")
    if not args.list:
        stem = os.path.splitext(os.path.basename(args.input))[0]
        qrc = _write_qrc(files, args.outdir, stem)
        print(f"\nextracted to {args.outdir}")
        print(f"repack recipe: rcc -binary {os.path.basename(qrc)} -o {stem}.rcc")


if __name__ == "__main__":
    main()
