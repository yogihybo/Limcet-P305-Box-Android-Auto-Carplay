#!/usr/bin/env python3
"""
diagnose_bootloader_hang.py -- Traces the permanent hang found when hardware-testing
live_factory_bootloader_12k_reconstructed.bin on the spare STM32F105 board.

This is a DIAGNOSTIC tool, not a claimed fix. It does NOT modify
tools/patch_factory_bootloader_live.py or its FACTORY_PATCH_MAP -- that file
belongs to the reconstruction work in progress. This script produces a
separate, clearly-labeled variant binary for a single, isolated hardware
experiment.

=== Confirmed on real silicon (spare STM32F105RBT6 board, this session) ===
Flashing live_factory_bootloader_12k_reconstructed.bin and letting it free-run
leaves the CPU permanently parked at PC=0x08000228 (Thread mode, no fault
flags set, RCC/USART2 registers still at power-on-reset defaults -- clock
setup and UART init never happen). Sampled 5x over ~4s: identical every time.

=== Root cause chain ===
1. 0x08000228 is one of the 7 words FACTORY_PATCH_MAP guessed for the
   0x08000200-0x08000234 block, as "SysTick_Handler (nop; b .)" -- i.e. an
   intentional infinite self-loop trap.

2. That specific guess is strongly *corroborated* by real silicon: the raw
   (pre-reconstruction) dump genuinely contains the IDENTICAL "b PC; nop"
   pattern, unpatched, at 3 directly adjacent addresses (0x08000210,
   0x08000214, 0x08000218 -- confirmed via raw==reconstructed byte compare).
   0x0800021C/0x08000220/0x08000224/0x08000228 continue the exact same
   4-byte-repeating "b .; nop" pattern the patcher guessed. Locally, this is
   a well-supported guess, not an arbitrary one.

3. BUT: a full, alignment-correct disassembly of the whole bootloader
   (starting at 0x08000200, the genuine post-vector-table code boundary --
   NOT from 0x08000000, which includes the vector table and desyncs
   Thumb decoding) finds exactly ONE control-flow instruction anywhere in
   the image that targets this trap block: a real `BL` at 0x08000332,
   which is GENUINELY EXTRACTED silicon data (raw == reconstructed, not a
   patched/guessed word). It unconditionally calls into 0x08000228 as a
   normal (non-error-path) step immediately following the PLL-lock /
   SYSCLK-switch clock routine (0x080002FE-0x0800032E, also genuinely
   extracted and cleanly disassembles as a standard "wait for PLLRDY, wait
   for SWS==PLL" sequence ending in `pop {r2,r3,pc}`).

4. A real, working factory bootloader cannot unconditionally call into a
   permanent trap on every single boot -- the real vehicle unit boots
   successfully today. So this is a genuine contradiction, and given (2)'s
   strong 3-neighbor corroboration for the CALLEE content, the more likely
   culprit is the CALLER encoding at 0x08000332-0x08000335 (bytes
   F7FF FF79), not the callee.

5. This lines up with a *pre-existing, independently-documented* red flag:
   docs/SESSION_HANDOFF_2026-09-13.md's caution section, from a PRIOR
   session using an earlier capture, already flagged 0x08000332 by name as
   a "vector corruption... branches into literal pools (INVSTATE
   UsageFault at 0x08000332)". Two different capture attempts, two
   different wrong decodings, same exact address -- consistent with this
   being a specific, repeatable weak/unreliable read location for the
   CVE-2020-8004 side channel, not a one-off fluke.

=== What this script does ===
Produces live_factory_bootloader_12k_HANGFIX_DIAGNOSTIC.bin: identical to
the reconstructed image, except the BL at 0x08000332 is replaced with
`nop; nop` (0xBF00 0xBF00), turning the wrapper at 0x08000330 into a no-op
(push r4,lr / nop / nop / pop r4,pc). This is NOT a claim about the real
factory bytes -- it is a controlled experiment to test the hypothesis: if
the hang is caused solely by this one bad call, skipping it should let the
rest of the boot sequence (HSE/PLL already locked by the preceding routine,
FLASH_ACR, USART2 init, etc.) proceed normally. If the CPU instead hangs or
faults somewhere else, that tells us the real bug is elsewhere and this
hypothesis is wrong.
"""

import struct
import sys

SRC = "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_reconstructed.bin"
OUT = "hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_HANGFIX_DIAGNOSTIC.bin"
BASE = 0x08000000
PATCH_ADDR = 0x08000332

def main():
    with open(SRC, "rb") as f:
        data = bytearray(f.read())

    off = PATCH_ADDR - BASE
    before = struct.unpack_from("<I", data, off)[0]
    print(f"Address 0x{PATCH_ADDR:08X} before: 0x{before:08X} (bl #0x8000228)")

    # NOP NOP (0xBF00 0xBF00) -- skips the call entirely, harmless single-step.
    struct.pack_into("<I", data, off, 0xBF00BF00)
    after = struct.unpack_from("<I", data, off)[0]
    print(f"Address 0x{PATCH_ADDR:08X} after:  0x{after:08X} (nop; nop -- DIAGNOSTIC ONLY)")

    with open(OUT, "wb") as f:
        f.write(data)
    print(f"\nWrote {OUT} ({len(data)} bytes)")
    print("This is a diagnostic variant, NOT a proposed factory-accurate fix.")

if __name__ == "__main__":
    sys.exit(main())
