#!/usr/bin/env python3
"""Sanity-check the reconstructed factory MCU images against their patch maps.

Two checks over every gap-filled (reconstructed) word, for both the
bootloader and the application (see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md
sections 32-33):

1. Annotation check: each patch-map entry's word is decoded in context
   (every instruction alignment overlapping it) and its mnemonics are
   compared with the entry's own description. A word whose bytes don't
   decode to what its annotation says is flagged -- this is how the
   byte-order errors (each halfword's bytes written in display order
   instead of little-endian) were found.
2. Call-target check: every BL that overlaps a reconstructed byte must
   target an address that silicon-read code also calls; otherwise it's
   tested with first/second/both halfwords byte-swapped and reported if a
   swap would produce a known target. A reconstructed BL whose target no
   silicon-read code calls must land on a plausible function entry (a push,
   or the instruction after a return/branch) -- this caught a BL into the
   middle of a function (section 34).
3. Entry check: the halfword before every function entry that silicon-read
   code calls must end the previous function (return, branch, nop pad) or be
   literal data, if it was reconstructed -- this caught a function whose
   epilogue and the next function's prologue were both missing (section 34).

Exit status 0 = clean. Needs `capstone` (pip install capstone).

Usage: python3 tools/verify_mcu_reconstruction.py
"""
import importlib.util, os, re, struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMPS = os.path.join(ROOT, "hardware/MCU/live_dumps/vehicle_live_2026-09-14")
TARGETS = (
    ("BOOT", "live_factory_bootloader_12k", "patch_factory_bootloader_live.py", "FACTORY_PATCH_MAP", 0x08000000),
    ("APP", "live_factory_app_52k", "patch_factory_app_live.py", "FACTORY_APP_PATCH_MAP", 0x08003000),
)
MNEM = set(("push pop bl blx b bx ldr ldrb ldrh ldrsb ldrsh str strb strh mov movs movw movt add adds sub "
            "subs cmp cmn cbz cbnz it ite itt orr orrs and ands eor eors bic bics lsl lsls lsr lsrs asr asrs "
            "mul muls udiv sdiv uxtb uxth sxtb sxth tst nop tbb tbh ldm ldmia stm stmia stmdb mvn mvns rsb rsbs "
            "bfi ubfx sbfx mla mls adr cpsid cpsie dsb isb dmb clz rev umull smull neg negs ldrd strd").split())
COND = r"(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)$"
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)


def norm(m):
    return m.lower().replace(".w", "").replace(".n", "")


def expected_mnemonics(desc):
    out = []
    for part in re.sub(r"\(.*?\)", "", desc).split("/"):
        tok = part.split()
        if not tok:
            continue
        t = norm(tok[0].rstrip(","))
        if t in MNEM or (t.startswith("b") and re.sub(COND, "", t[1:]) == "" and len(t) > 1):
            out.append(t)
    return out


def load_map(tool, var):
    spec = importlib.util.spec_from_file_location("m_" + var, os.path.join(ROOT, "tools", tool))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return getattr(mod, var)


def check_annotations(img, base, pmap):
    bad = []
    for addr, (word, desc) in sorted(pmap.items()):
        cur = struct.unpack_from("<I", img, addr - base)[0]
        if cur != word:
            bad.append(f"0x{addr:08X}: image 0x{cur:08X} != map 0x{word:08X} (regenerate the image)")
            continue
        exp = expected_mnemonics(desc)
        if not exp:
            continue
        ok = False
        for start in (addr - 2, addr):
            o = start - base
            got = [norm(i.mnemonic) for i in md.disasm(img[o:o + 10], start)
                   if i.address < addr + 4 and i.address + i.size > addr]
            if all(any(g == e or g.startswith(e) or e.startswith(g) for g in got) for e in exp):
                ok = True
                break
        if not ok:
            bad.append(f"0x{addr:08X}: decodes to {got}, annotation says {exp} -- {desc}")
    return bad


def bl_target(addr, h1, h2):
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xD000) != 0xD000:
        return None
    s = (h1 >> 10) & 1
    i1 = 1 - (((h2 >> 13) & 1) ^ s)
    i2 = 1 - (((h2 >> 11) & 1) ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s:
        off -= 1 << 25
    return addr + 4 + off


def ends_function(h):
    # bx lr / bx rN, pop {..., pc}, b.n, nop, zero padding
    return (h & 0xFF87) == 0x4700 or (h & 0xFF00) == 0xBD00 or (h & 0xF800) == 0xE000 or h in (0xBF00, 0)


def ends_before(hw, o):
    # The instruction ending at offset o: a 16-bit terminator, or pop.w {..., pc}
    return ends_function(hw(o - 2)) or (o >= 4 and hw(o - 4) == 0xE8BD and hw(o - 2) & 0x8000)


def is_push(h):
    return (h & 0xFE00) == 0xB400 or h == 0xE92D


def is_data(addr, pmap):
    for a in (addr & ~3, (addr & ~3) - 4):
        if a in pmap and re.search(r"literal|address|table|pool|data", pmap[a][1], re.I):
            return True
    return False


def check_calls(img, raw, base, pmap):
    gap = [img[i] != raw[i] for i in range(len(img))]
    hw = lambda o: img[o] | (img[o + 1] << 8)
    hws = lambda o: img[o + 1] | (img[o] << 8)
    known = set()
    for o in range(0, len(img) - 3, 2):
        if not any(gap[o:o + 4]):
            t = bl_target(base + o, hw(o), hw(o + 2))
            if t is not None and base <= t < base + len(img):
                known.add(t)
    bad = []
    for o in range(0, len(img) - 3, 2):
        t0 = bl_target(base + o, hw(o), hw(o + 2))
        if not any(gap[o:o + 4]) or t0 in known:
            continue
        for h1, h2 in ((hws(o), hw(o + 2)), (hw(o), hws(o + 2)), (hws(o), hws(o + 2))):
            t = bl_target(base + o, h1, h2)
            if t in known:
                bad.append(f"0x{base + o:08X}: byte-swapped BL to 0x{t:08X}")
                break
        else:
            if t0 is not None and base < t0 < base + len(img) and not (
                    is_push(hw(t0 - base)) or ends_before(hw, t0 - base)):
                bad.append(f"0x{base + o:08X}: BL to 0x{t0:08X}, which no silicon-read code calls "
                           f"and which isn't a function entry")
    for t in sorted(known):
        o = t - base - 2
        if o >= 0 and any(gap[o:o + 2]) and not ends_before(hw, o + 2) and not is_data(base + o, pmap):
            bad.append(f"0x{t:08X}: called from silicon-read code, but the reconstructed halfword "
                       f"before it (0x{hw(o):04X}) doesn't end a function")
    return bad, len(known)


def main():
    failed = False
    for name, stem, tool, var, base in TARGETS:
        img = open(os.path.join(DUMPS, stem + "_reconstructed.bin"), "rb").read()
        raw = open(os.path.join(DUMPS, stem + "_raw.bin"), "rb").read()
        pmap = load_map(tool, var)
        ann = check_annotations(img, base, pmap)
        calls, nknown = check_calls(img, raw, base, pmap)
        print(f"{name}: annotation check {len(ann)} flagged; call-target check {len(calls)} flagged "
              f"({nknown} silicon-read call targets)")
        for line in ann + calls:
            print("   " + line)
        failed |= bool(ann or calls)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
