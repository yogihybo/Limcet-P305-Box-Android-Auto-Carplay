#!/usr/bin/env python3
"""Deconstruct the head unit's Qt UI binary (MsnCoreApp) for targeted patching.

MsnCoreApp is a stripped-at-ship, but the vendor also left an *unstripped* build
(`MsnCoreApp-original` / `-auth`) whose symbol table names every C++ method. The
app itself was compiled without DWARF (only the bundled WebRTC audio library has
debug info), so there is no source to recover — but the symbol table + machine
code are enough to locate any screen's layout code and read the literal geometry
constants that a patch would change.

This tool:
  --list [filter]      list app functions (name, address, size), demangled
  --func <symbol>      disassemble one function: resolve every Qt call (via the
                       PLT), and dump the immediate constants it loads — for a
                       setupUi / constructor these are the widget x/y/w/h values.

Layout in this app comes in two flavours, both reachable here:
  * uic-generated dialogs (Ui_*::setupUi) — a 1:1 translation of a .ui file;
    setGeometry(QRect(x,y,w,h)) with literal coords.
  * hand-coded screens (MsnCoreApp::* etc.) — QWidget::setGeometry / QBoxLayout
    built in C++; same literal coords, just not from a .ui.

To move/resize a control you patch those immediates in .text and repack the
rootfs. See docs/MSNCOREAPP_DECONSTRUCTION.md.

Requires: pyelftools, capstone, cxxfilt  (pip install pyelftools capstone cxxfilt)
Usage:
    python msncore_analyze.py <MsnCoreApp-original> --list [substr]
    python msncore_analyze.py <MsnCoreApp-original> --func Ui_VersionDialog::setupUi
"""
import sys, argparse
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_OP_IMM
def _mini_demangle(n):
    """Recover just the qualified name (Class::sub::method) from an Itanium
    mangled symbol. Ignores template/argument types — enough to navigate.
    Falls back to the raw symbol on anything it doesn't understand."""
    if not n.startswith("_Z"):
        return n
    s = n[2:]
    nested = s.startswith("N")
    if nested:
        s = s[1:]
        while s[:1] in ("r", "V", "K"):   # CV-qualifiers
            s = s[1:]
    parts, last = [], ""
    while s:
        if nested and s[0] == "E":
            break
        if s[0].isdigit():
            i = 0
            while i < len(s) and s[i].isdigit():
                i += 1
            ln = int(s[:i]); last = s[i:i + ln]; parts.append(last); s = s[i + ln:]
        elif s[0] == "C":            # C1/C2/C3 constructor
            parts.append(parts[-1] if parts else "?"); s = s[2:]
        elif s[0] == "D":            # D0/D1/D2 destructor
            parts.append("~" + (parts[-1] if parts else "?")); s = s[2:]
        elif s[0] == "I":            # template args — skip to matching E
            depth = 0
            while s:
                if s[0] == "I": depth += 1
                elif s[0] == "E":
                    depth -= 1
                    if depth == 0: s = s[1:]; break
                s = s[1:]
        else:
            break
    return "::".join(parts) if parts else n


try:
    import cxxfilt
    def dm(n):
        try:
            out = cxxfilt.demangle(n)
            return out if out != n else _mini_demangle(n)   # cxxfilt no-op fallback
        except Exception:
            return _mini_demangle(n)
except ImportError:
    def dm(n): return _mini_demangle(n)


class Bin:
    def __init__(self, path):
        self.e = ELFFile(open(path, "rb"))
        self.symtab = self.e.get_section_by_name(".symtab")
        if self.symtab is None:
            sys.exit("no .symtab — this must be the UNSTRIPPED build "
                     "(MsnCoreApp-original / -auth), not the shipped MsnCoreApp")
        self.text = self.e.get_section_by_name(".text")
        self._plt_names()
        self._sym_names()

    def _sym_names(self):
        self.byname = {}
        self.byaddr = {}
        for s in self.symtab.iter_symbols():
            if s["st_info"]["type"] == "STT_FUNC" and s["st_value"]:
                self.byname[s.name] = (s["st_value"], s["st_size"])
                self.byaddr.setdefault(s["st_value"] & ~1, s.name)

    def _plt_names(self):
        self.plt = {}
        relplt = self.e.get_section_by_name(".rel.plt")
        pltsec = self.e.get_section_by_name(".plt")
        dyn = self.e.get_section_by_name(".dynsym")
        if not (relplt and pltsec and dyn):
            return
        dsyms = list(dyn.iter_symbols())
        base = pltsec["sh_addr"] + 20          # skip PLT[0] resolver
        for i, r in enumerate(relplt.iter_relocations()):
            self.plt[base + i * 12] = dsyms[r["r_info_sym"]].name

    def resolve(self, addr):
        nm = self.plt.get(addr) or self.byaddr.get(addr & ~1)
        return dm(nm) if nm else None

    def code_at(self, addr, size):
        b = self.text["sh_addr"]
        return self.text.data()[addr - b: addr - b + size]


def list_funcs(b, filt):
    rows = []
    for name, (addr, size) in b.byname.items():
        d = dm(name)
        if filt and filt.lower() not in d.lower():
            continue
        # skip the statically-linked WebRTC DSP + Qt internals
        if any(k in name for k in ("webrtc", "rtc")) and "MsnCore" not in name:
            continue
        rows.append((addr, size, d))
    for addr, size, d in sorted(rows):
        print(f"  {addr:#010x}  {size:5}  {d}")
    print(f"\n{len(rows)} functions")


def disasm(b, symbol):
    ent = b.byname.get(symbol)
    if ent is None:  # try matching demangled
        for n, v in b.byname.items():
            if dm(n) == symbol or dm(n).startswith(symbol + "("):
                ent = v; symbol = n; break
    if ent is None:
        sys.exit(f"symbol not found: {symbol}")
    addr, size = ent
    thumb = addr & 1
    addr &= ~1
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    md.detail = True
    print(f"# {dm(symbol)}  @ {addr:#x}  ({size} bytes, {'Thumb' if thumb else 'ARM'})\n")
    calls, imms = [], []
    for ins in md.disasm(b.code_at(addr, size), addr):
        if ins.mnemonic in ("bl", "blx") and ins.operands and ins.operands[0].type == CS_OP_IMM:
            nm = b.resolve(ins.operands[0].imm)
            if nm:
                calls.append(nm)
                print(f"  {ins.address:#08x}  call {nm}")
        if ins.mnemonic in ("mov", "movw") and ins.operands and ins.operands[-1].type == CS_OP_IMM:
            v = ins.operands[-1].imm
            if 0 < v < 4096:
                imms.append((ins.address, v))
    print("\n# geometry / size immediate constants (candidate x / y / w / h / spacing):")
    print("  ", [v for _, v in imms])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--list", nargs="?", const="", metavar="SUBSTR")
    ap.add_argument("--func", metavar="SYMBOL")
    a = ap.parse_args()
    b = Bin(a.binary)
    if a.func:
        disasm(b, a.func)
    else:
        list_funcs(b, a.list or "")


if __name__ == "__main__":
    main()
