#!/usr/bin/env python3
"""Read-only snapshot of the car's MCU (CAN filters first), in one SWD attach.

Uses tools/pico_stm32_vehicle_readonly.cfg (no flash bank: no erase, unlock or
option-byte commands exist) and only issues memory reads -- no halt, no reset.
Under RDP1 the debugger can still read SRAM and peripheral registers; only
flash is blocked. Attaching does stop the MCU: tested on an RDP1 bench board
(findings section 49), the app HardFaults a moment after the attach, the ~2 s
IWDG resets it, and the bootloader then sits in its HardFault loop until a power
cycle. So everything runs in one OpenOCD call (~2 s), most important first:

  1. CAN1 filter banks + filter control (the two unknown dispatch-table IDs,
     findings section 48). Filter-bank contents survive a reset; the control
     registers don't.
  2. CAN1/CAN2 control and bit timing, RCC, FLASH (OBR/WRPR), GPIO A-E, AFIO,
     EXTI, USART1-3, I2C1/2, IWDG, timers, SysTick, NVIC, SCB.
  3. All 64 KB of SRAM (live state: CAN receive ring at 0x20000270, settings,
     board variant at 0x200001F2).
  4. The filter banks, RCC_CSR and DHCSR again, to show whether the MCU reset
     part-way through.

Writes <out>/regs.txt (raw OpenOCD output), <out>/sram.bin, and prints the
decoded CAN filter IDs with a self-check against the four silicon-read IDs.

Usage:
  python3 tools/vehicle_snapshot_readonly.py [--out DIR]
  python3 tools/vehicle_snapshot_readonly.py --decode DIR   # re-decode a capture
"""
import argparse
import datetime
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "tools", "pico_stm32_vehicle_readonly.cfg")

# (label, address, words) -- order matters, see docstring.
FIRST = [
    ("DHCSR", 0xE000EDF0, 1),
    ("RCC_CSR", 0x40021024, 1),
    ("CAN1 filter control FMR..FA1R", 0x40006600, 8),
    ("CAN1 filter banks 0-27", 0x40006640, 56),
    ("CAN1 MCR..BTR", 0x40006400, 8),
    ("CAN2 MCR..BTR", 0x40006800, 8),
]
REST = [
    ("RCC CR..AHBRSTR", 0x40021000, 11),
    ("RCC CFGR2", 0x4002102C, 1),
    ("FLASH ACR", 0x40022000, 1),
    ("FLASH OBR, WRPR", 0x4002201C, 2),
    ("GPIOA", 0x40010800, 7),
    ("GPIOB", 0x40010C00, 7),
    ("GPIOC", 0x40011000, 7),
    ("GPIOD", 0x40011400, 7),
    ("GPIOE", 0x40011800, 7),
    ("AFIO", 0x40010000, 8),
    ("EXTI", 0x40010400, 6),
    ("USART1", 0x40013800, 7),
    ("USART2", 0x40004400, 7),
    ("USART3", 0x40004800, 7),
    ("I2C1", 0x40005400, 9),
    ("I2C2", 0x40005800, 9),
    ("IWDG PR, RLR, SR", 0x40003004, 3),
    ("TIM1", 0x40012C00, 20),
    ("TIM2", 0x40000000, 20),
    ("TIM3", 0x40000400, 20),
    ("TIM4", 0x40000800, 20),
    ("TIM5", 0x40000C00, 20),
    ("TIM6", 0x40001000, 20),
    ("TIM7", 0x40001400, 20),
    ("ADC1", 0x40012400, 20),
    ("DMA1", 0x40020000, 36),
    ("SysTick", 0xE000E010, 4),
    ("NVIC ISER", 0xE000E100, 3),
    ("NVIC IPR", 0xE000E400, 17),
    ("SCB CPUID..BFAR", 0xE000ED00, 15),
]
LAST = [
    ("again: CAN1 filter banks 0-1", 0x40006640, 4),
    ("again: RCC_CSR", 0x40021024, 1),
    ("again: DHCSR", 0xE000EDF0, 1),
]
SRAM = (0x20000000, 0x10000)


def parse(text):
    """{address: word} from OpenOCD mdw output; later reads of an address win."""
    words = {}
    for m in re.finditer(r"^0x([0-9a-f]{8}):((?: [0-9a-f]{8})+)", text, re.M):
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[base + 4 * i] = int(w, 16)
    return words


def decode(text):
    head, _, tail = text.partition("again:")
    first = parse(head)
    w = lambda a: first.get(a)
    print("\nCAN1 filters")
    fmr, fm1r, fs1r, ffa1r, fa1r = (w(0x40006600), w(0x40006604), w(0x4000660C),
                                    w(0x40006614), w(0x4000661C))
    if None in (fmr, fm1r, fs1r, fa1r):
        print("  [!] filter control registers not read")
        return 1
    print(f"  FMR {fmr:08X} (CAN2 start bank {(fmr >> 8) & 0x3F})  FM1R {fm1r:08X}  "
          f"FS1R {fs1r:08X}  FFA1R {ffa1r:08X}  FA1R {fa1r:08X}")
    if fmr & 1 or fa1r == 0:
        print("  [!] control registers look reset (FINIT set or no active filters);"
              " bank contents below may still be valid")
    ids = {}
    for bank in range(28):
        r1, r2 = w(0x40006640 + 8 * bank), w(0x40006644 + 8 * bank)
        if r1 is None:
            continue
        active, listmode, scale32 = (fa1r >> bank) & 1, (fm1r >> bank) & 1, (fs1r >> bank) & 1
        if not active and bank > 1:
            continue
        desc = f"  bank {bank:2d}: {r1:08X} {r2:08X}  {'active' if active else 'inactive'}, " \
               f"{'list' if listmode else 'mask'}, {'32' if scale32 else '16'}-bit"
        if not scale32:
            halves = [r1 & 0xFFFF, r1 >> 16, r2 & 0xFFFF, r2 >> 16]
            bank_ids = [h >> 5 for h in halves]
            desc += "  IDs " + " ".join(f"0x{i:03X}" for i in bank_ids)
            if bank in (0, 1):
                ids[bank] = (r1, r2)
        print(desc)
    if len(ids) == 2:
        # SPL 16-bit list mode: FR1 = MaskIdLow<<16 | IdLow, FR2 = MaskIdHigh<<16 | IdHigh.
        # Factory set-up (0x08008F92): IdHigh, IdLow, MaskIdHigh, MaskIdLow =
        # table[0..3] for bank 0 and table[4], table[5], X, table[4] for bank 1.
        (a1, a2), (b1, b2) = ids[0], ids[1]
        slots = [("id0", a2 & 0xFFFF, 0x396), ("id1", a1 & 0xFFFF, 0x025),
                 ("id2", a2 >> 16, None), ("id3", a1 >> 16, 0x1D0),
                 ("id4", b2 & 0xFFFF, 0x622), ("id5", b1 & 0xFFFF, None),
                 ("X  ", b2 >> 16, None), ("id4", b1 >> 16, 0x622)]
        print("\n  Dispatch table (0x0800B9F4) as programmed into the filters:")
        ok = True
        for name, half, expect in slots:
            sid, rtr, ide = half >> 5, (half >> 4) & 1, (half >> 3) & 1
            note = ("UNKNOWN in the reconstruction -- this is the answer" if expect is None and name != "X  "
                    else "reconstruction reads past the table here (0x08009006)" if expect is None
                    else "matches silicon" if sid == expect else f"EXPECTED 0x{expect:03X}")
            ok &= expect is None or sid == expect
            print(f"    {name} 0x{sid:03X}  (raw {half:04X}{', RTR' if rtr else ''}{', IDE' if ide else ''})  {note}")
        print("  self-check: " + ("PASS -- the four silicon-read IDs are where expected, so id2/id5 are real"
                                 if ok else "FAIL -- registers don't hold the factory set-up; don't trust id2/id5"))
    later = parse(tail)
    if later:
        same = all(later.get(a) == first.get(a) for a in range(0x40006640, 0x40006650, 4))
        print(f"\n  filter banks unchanged at the end: {same}")
        print(f"  RCC_CSR start {first.get(0x40021024, 0):08X} end {later.get(0x40021024, 0):08X}  "
              f"DHCSR start {first.get(0xE000EDF0, 0):08X} end {later.get(0xE000EDF0, 0):08X}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(
        ROOT, "hardware", "MCU", "live_dumps",
        "vehicle_snapshot_" + datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")))
    ap.add_argument("--decode", metavar="DIR", help="decode an existing capture")
    args = ap.parse_args()

    if args.decode:
        return decode(open(os.path.join(args.decode, "regs.txt")).read())

    os.makedirs(args.out, exist_ok=True)
    sram_path = os.path.join(args.out, "sram.bin")
    cmds = ["init"]
    for group in (FIRST, REST):
        for label, addr, n in group:
            cmds += [f"echo {{# {label}}}", f"mdw 0x{addr:08X} {n}"]
    cmds += [f"dump_image {{{sram_path}}} 0x{SRAM[0]:08X} 0x{SRAM[1]:X}"]
    for label, addr, n in LAST:
        cmds += [f"echo {{# {label}}}", f"mdw 0x{addr:08X} {n}"]
    cmds += ["shutdown"]

    argv = ["openocd", "-f", CFG]
    for c in cmds:
        argv += ["-c", c]
    res = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    text = res.stdout + res.stderr
    with open(os.path.join(args.out, "regs.txt"), "w") as f:
        f.write(text)
    print(f"capture: {args.out}/regs.txt" +
          (f", sram.bin ({os.path.getsize(sram_path)} bytes)" if os.path.exists(sram_path) else ", NO sram.bin"))
    if "Error" in text:
        print("[!] OpenOCD reported errors -- see regs.txt")
    return decode(text)


if __name__ == "__main__":
    sys.exit(main())
