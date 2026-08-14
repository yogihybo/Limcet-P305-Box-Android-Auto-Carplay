#!/usr/bin/env python3
"""decode-pins.py -- annotate a pin-dump.sh capture with function names.

pin-dump.sh only prints raw {reg, offset, mask} -> PVAL numbers -- it has
no notion of what a given PVAL *means* for a given pin. This script cross-
references those numbers against every named ark,pins group in
ark1668-pinctrl.dtsi (the same "function X group Y" info the live debugfs
pinmux-pins route gives on our own board, reconstructed here for stock
firmware, which has no such interface -- see
docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md and docs/I2C_GPIO0_LCD_PIN_CONFLICT.md).

Usage:
    decode-pins.py <path-to-ark1668-pinctrl.dtsi> <path-to-pin-dump-capture>

The capture is the raw stdout of pin-dump.sh, one "pin N: reg=.. offset=..
mask=.. -> PVAL=.." line per pin (extra terminal noise like a shell prompt
line is tolerated and skipped).
"""
import re
import sys

# Pins with a fixed board-level purpose that isn't expressed as a named
# ark,pins group in the dtsi (plain GPIO wiring to a specific device).
KNOWN_EXTRA = {
    9: "i2c-gpio-1 SDA (BD37033) -- board wiring, no named pinctrl group",
    121: "i2c-gpio-1 SCL (BD37033) -- board wiring, no named pinctrl group",
    2: "also i2c-gpio-0 SDA (rn6752/gt911) -- board wiring",
    3: "also i2c-gpio-0 SCL (rn6752/gt911) -- board wiring",
}


def parse_pinctrl_dtsi(path):
    with open(path) as f:
        content = f.read()
    groups = re.findall(
        r"(\w+):\s*([\w-]+)\s*\{\s*ark,pins\s*=\s*<(.*?)>;", content, re.S
    )
    func = {}
    for _symbol, label, body in groups:
        for m in re.finditer(
            r"ARK_PBANK_(\d+)\s+(\d+)\s+ARK_PVAL_(\d+)\s*(?:/\*\s*(.*?)\s*\*/)?", body
        ):
            bank, offset, pval, comment = m.groups()
            global_pin = int(bank) * 32 + int(offset)
            func.setdefault(global_pin, {})[int(pval)] = (label, comment or "")
    return func


def parse_dump(path):
    entries = []
    pat = re.compile(
        r"pin\s+(\d+):\s*reg=(\S+)\s*offset=\s*(\d+)\s*mask=(\S+)\s*->\s*PVAL=(\d+)"
    )
    with open(path) as f:
        for line in f:
            m = pat.match(line.strip())
            if m:
                pin, reg, off, mask, pval = m.groups()
                entries.append((int(pin), reg, int(off), mask, int(pval)))
    return entries


def annotate(func, entries):
    lines = []
    for pin, reg, off, mask, pval in entries:
        opts = func.get(pin, {})
        if pval in opts:
            label, comment = opts[pval]
            name = f"{comment or '?'} (group {label})"
        elif opts:
            avail = ", ".join(f"PVAL={p}:{c or l}" for p, (l, c) in sorted(opts.items()))
            name = f"UNRECOGNIZED PVAL={pval} (known options: {avail})"
        else:
            name = KNOWN_EXTRA.get(
                pin, "no named pinctrl group (plain GPIO or undocumented function)"
            )
        lines.append(
            f"pin {pin:3d} (reg={reg} off={off:2d} mask={mask}): PVAL={pval} -> {name}"
        )
    return lines


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    func = parse_pinctrl_dtsi(sys.argv[1])
    entries = parse_dump(sys.argv[2])
    for line in annotate(func, entries):
        print(line)
