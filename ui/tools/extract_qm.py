#!/usr/bin/env python3
"""
extract_qm.py - Decompile Qt .qm translation files to readable text

Qt .qm format:
  Magic: 0x3C B8 64 18 CA EF 9C 95 CD 21 1C BF 60 A1 56 D7
  Followed by sections: each section has a 1-byte type, 4-byte length, then data.
  Section types:
    0x42 ('B') = Messages
    0x69 ('i') = Contexts (older format)
    0x2F ('/') = NumerusRules
    0x88       = Dependencies

Message block binary format (inside section 0x42):
  The messages section uses a nested structure. We parse key strings and
  translations from the message hash tables.

For simplicity this script uses the msgfmt/lconvert approach if available,
otherwise falls back to binary extraction of visible strings.

Usage:
  python extract_qm.py lang_en.qm
  python extract_qm.py *.qm -o qm_extracted/
"""

import argparse
import struct
import sys
from pathlib import Path

QM_MAGIC = bytes([0x3C, 0xB8, 0x64, 0x18, 0xCA, 0xEF, 0x9C, 0x95,
                  0xCD, 0x21, 0x1C, 0xBF, 0x60, 0xA1, 0xBD, 0xDD])

# Section type IDs
SEC_MESSAGES    = 0x42
SEC_CONTEXTS    = 0x69
SEC_HASH_TABLE  = 0x13  # Qt4 message offset table

# Qt4 message tag IDs (qtranslator.cpp)
TAG_END          = 1   # end of message record
TAG_SOURCE16     = 2   # obsolete UTF-16 source
TAG_TRANSLATION  = 3   # UTF-16 translation (0xFFFFFFFF = use source)
TAG_CONTEXT16    = 4   # obsolete UTF-16 context
TAG_OBSOLETE1    = 5
TAG_SOURCE_TEXT  = 6   # ASCII source text
TAG_CONTEXT      = 7   # ASCII context name
TAG_COMMENT      = 8   # ASCII comment
TAG_OBSOLETE2    = 20


def read_section(data, pos):
    if pos >= len(data):
        return None, pos
    tag = data[pos]
    length = struct.unpack_from('>I', data, pos + 1)[0]
    body = data[pos + 5: pos + 5 + length]
    return (tag, body), pos + 5 + length


def parse_messages_section(body):
    """Extract (context, source, translation) triples from the Messages section."""
    results = []
    pos = 0
    context = ''
    source = ''
    translation = None

    def flush():
        nonlocal context, source, translation
        if source is not None:
            t = translation if translation is not None else source
            results.append((context, source, t))
        context = source = ''

    while pos < len(body):
        tag = body[pos]
        pos += 1
        if tag == TAG_END:
            flush()
            translation = None
            continue

        if pos + 4 > len(body):
            break
        length = struct.unpack_from('>I', body, pos)[0]
        pos += 4

        if tag == TAG_TRANSLATION:
            if length == 0xFFFFFFFF:
                # no translation — use source text as-is
                translation = None
            else:
                raw = body[pos: pos + length]
                try:
                    translation = raw.decode('utf-16-be')
                except Exception:
                    translation = raw.decode('latin-1', errors='replace')
                pos += length
            continue

        raw = body[pos: pos + length]
        pos += length

        if tag in (TAG_SOURCE_TEXT, TAG_CONTEXT, TAG_COMMENT,
                   TAG_SOURCE16, TAG_CONTEXT16):
            try:
                if tag in (TAG_SOURCE16, TAG_CONTEXT16):
                    text = raw.decode('utf-16-be', errors='replace')
                else:
                    text = raw.decode('ascii', errors='replace')
            except Exception:
                text = raw.decode('latin-1', errors='replace')

            if tag == TAG_SOURCE_TEXT:
                source = text
            elif tag == TAG_CONTEXT:
                context = text
        # skip TAG_OBSOLETE1/2 and unknowns silently

    flush()
    return results


def decompile_qm(path: Path):
    data = path.read_bytes()
    if data[:16] != QM_MAGIC:
        print(f"  ERROR: not a .qm file (bad magic)")
        return []

    pos = 16
    sections = []
    while pos < len(data):
        result, pos = read_section(data, pos)
        if result is None:
            break
        sections.append(result)

    translations = []
    for tag, body in sections:
        if tag == SEC_CONTEXTS:  # 0x69 = Messages data in Qt4
            translations = parse_messages_section(body)
            break

    return translations


def main():
    ap = argparse.ArgumentParser(description='Decompile Qt .qm translation files')
    ap.add_argument('qm', nargs='+', help='.qm files to decompile')
    ap.add_argument('-o', '--out', default='qm_extracted',
                    help='Output directory (default: qm_extracted/)')
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    for pattern in args.qm:
        paths = list(Path('.').glob(pattern)) or [Path(pattern)]
        for p in sorted(paths):
            print(f"\n{p.name}:")
            translations = decompile_qm(p)
            if not translations:
                print("  (no translations extracted)")
                continue

            out_path = out_dir / (p.stem + '.txt')
            lines = []
            for ctx, src, trans in translations:
                if ctx:
                    lines.append(f"[{ctx}]")
                lines.append(f"  source:      {src!r}")
                lines.append(f"  translation: {trans!r}")
                lines.append('')
            out_path.write_text('\n'.join(lines), encoding='utf-8')
            print(f"  {len(translations)} strings -> {out_path}")


if __name__ == '__main__':
    main()
