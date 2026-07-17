#!/usr/bin/env python3
"""find-unclaimed-pins.py -- list every pin from a pin-dump.sh capture whose
real hardware PVAL does not match ANY named ark,pins group in
ark1668-pinctrl.dtsi.

This replaces decode-pins.py's old KNOWN_EXTRA table, which just echoed
back our own prior guesses about which pins carry i2c-gpio-0/i2c-gpio-1
(BD37033/GT911/RN6752) rather than proving anything from the register
read itself (see the 2026-07-17 correction in the project history: pins
2/3/9 are genuinely, independently confirmed as LCD via their real PVAL
matching the DTSI's own r0/r1/r7 encoding, but pin 121's "BD37033 SCL"
label was never anything but that same guess re-printed).

This script draws no conclusions about *which* unclaimed pin belongs to
which device -- that needs a schematic or a live i2c-scan per candidate.
It only produces the honest candidate list: pins where the stock
hardware's live mux setting doesn't match anything our own DTS already
understands.

Usage:
    find-unclaimed-pins.py <path-to-ark1668-pinctrl.dtsi> <path-to-pin-dump-capture>
"""
import re
import sys


def parse_pinctrl_dtsi(path):
    with open(path) as f:
        content = f.read()
    groups = re.findall(
        r"(\w+):\s*([\w-]+)\s*\{\s*ark,pins\s*=\s*<(.*?)>;", content, re.S
    )
    # global_pin -> {pval: [(group_label, comment), ...]}
    func = {}
    for _symbol, label, body in groups:
        for m in re.finditer(
            r"ARK_PBANK_(\d+)\s+(\d+)\s+ARK_PVAL_(\d+)\s*(?:/\*\s*(.*?)\s*\*/)?", body
        ):
            bank, offset, pval, comment = m.groups()
            global_pin = int(bank) * 32 + int(offset)
            func.setdefault(global_pin, {}).setdefault(int(pval), []).append(
                (label, comment or "")
            )
    return func


def parse_dump(path):
    entries = []
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*pin\s+(\d+):.*PVAL=(\d+)", line)
            if m:
                entries.append((int(m.group(1)), int(m.group(2))))
    return entries


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    func = parse_pinctrl_dtsi(sys.argv[1])
    dump = parse_dump(sys.argv[2])

    print(f"{'pin':>4}  {'PVAL':>4}  status")
    print("-" * 70)
    unclaimed = []
    for pin, pval in dump:
        groups_for_pin = func.get(pin, {})
        if pval in groups_for_pin:
            names = ", ".join(f"{label} ({c})" if c else label for label, c in groups_for_pin[pval])
            print(f"{pin:>4}  {pval:>4}  claimed -> {names}")
        else:
            other = (
                f" (other PVALs defined on this pin: {sorted(groups_for_pin.keys())})"
                if groups_for_pin
                else " (no group anywhere defines this pin at all)"
            )
            print(f"{pin:>4}  {pval:>4}  UNCLAIMED{other}")
            unclaimed.append((pin, pval))

    print("-" * 70)
    print(f"{len(unclaimed)} unclaimed pin(s) -- candidates for undocumented board wiring:")
    for pin, pval in unclaimed:
        print(f"  pin {pin} (PVAL={pval})")


if __name__ == "__main__":
    main()
