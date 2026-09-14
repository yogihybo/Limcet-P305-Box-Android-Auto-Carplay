#!/usr/bin/env python3
"""
patch_factory_bootloader_live.py — Reconstructs the genuine Keil factory bootloader
extracted from the live Prado vehicle MCU (STM32F105RBT6).

Unlike clean-room patchers, this tool patches the stock factory Keil binary
by resolving the 113 gaps resulting from the 512-byte VTOR hardware alignment limit
under CVE-2020-8004 side-channel extraction.

Usage:
  python3 tools/patch_factory_bootloader_live.py <input_raw.bin> <output_reconstructed.bin>
"""

import sys
import os
import argparse
import hashlib
import struct

# Factory-specific reconstruction map based on Keil RealView MDK ARMCC v5 disassembly
# and STM32F10X_CL Standard Peripheral Library (SPL) v3.x matching:
FACTORY_PATCH_MAP = {
    # --- Literal Pool Filter Collision ---
    0x080004A8: (0x20000004, "movs r4, r0 / movs r0, #0 (Reset entry literal dropped by NOP filter)"),

    # --- Block 1: CMSIS Intrinsics, Exception Loops & Clocks (0x08000200 - 0x08000234) ---
    0x08000200: (0x47708808, "msr primask, r0 / bx lr (__set_PRIMASK completion)"),
    0x08000204: (0x4770BA40, "rev16 r0, r0 / bx lr (__REV16 helper)"),
    # REVISED 2026-09-14, v3 (docs/BOOTLOADER_HANG_TRACE_2026-09-14.md sections
    # 6-11): the original guess (4 more "nop; b ." trap-stub copies) hardware-
    # tested as a real, reproducible permanent hang.
    #
    # CRITICAL CORRECTION (v3, supersedes v2 below): v2 placed the prologue's
    # `push {r2,r3,lr}` at 0x0800021C, reasoning that SetSysClock() must begin
    # somewhere in this 4-word block. That was wrong about WHICH word --
    # the genuine (unpatched) call site at 0x08000332 is `bl #0x8000228`,
    # which means the function's real ENTRY POINT is 0x08000228 itself, not
    # 0x0800021C. 0x0800021C-0x08000224 are simply never reached via this
    # call path at all (confirmed via single-step trace: SP never changes
    # across them, proving no instruction there executes as part of this
    # call). Placing the push at 0x21C meant it silently never ran, leaving
    # SP untouched through the whole function and making `pop {r2,r3,pc}` at
    # 0x0800032E pop whatever pre-existing stack content happened to sit
    # above the wrapper's own frame (explains the earlier, very confusing
    # "0x00003000-ish" / "0x08004F50" / app-content-correlated pop values --
    # none of that was a real app-jump bug, it was reading stale frames left
    # by the wrapper/SystemInit's own already-pushed {r4,lr} pairs).
    #
    # v3 fix: the entire prologue must self-containedly fit in the single
    # 4-byte word at 0x08000228 (the real entry point) -- `push {r2,r3,lr}`
    # (2 bytes) + `movs r0,#0` (2 bytes), exactly 4 bytes. 0x0800021C/
    # 0x08000220/0x08000224 are restored to the original "nop; b ." trap-stub
    # guess (matching the 3 genuinely-extracted neighbors at 0x210/14/18) --
    # since they're unreached via the confirmed call path, their exact
    # content doesn't matter for this bug; kept as the best available guess
    # rather than invented content.
    #
    # v2's other findings still stand: the prologue doesn't need to touch
    # RCC_CR_HSEON itself (genuine code at 0x08000230-0x0800023A already does
    # that independently once 0x08000234 is fixed below), and no `sub sp`
    # is needed (the 2 locals reuse the pushed r2/r3 slots directly).
    #
    # HARDWARE-CONFIRMED (2026-09-14) with v2's prologue position: RCC->CR
    # read back as 0x0F036D83 -- HSEON/HSERDY/PLLON/PLLRDY all set, SYSCLK
    # switched to PLL. That clock result is independent of WHERE the push
    # sits (genuine downstream code drives it either way) so it should still
    # hold with v3; re-verify on next hardware pass.
    0x0800021C: (0xBF00E7FE, "nop; b . (unreached via the confirmed 0x08000332 call path; original trap-stub guess retained)"),
    0x08000220: (0xBF00E7FE, "nop; b . (unreached via the confirmed 0x08000332 call path; original trap-stub guess retained)"),
    0x08000224: (0xBF00E7FE, "nop; b . (unreached via the confirmed 0x08000332 call path; original trap-stub guess retained)"),
    0x08000228: (0x2000B50C, "push {r2,r3,lr} / movs r0,#0 (REAL SetSysClock() entry point -- bl #0x8000228 targets here directly)"),
    # BUG FIX 2026-09-14: original value 0xF4403080 has its Thumb halfwords
    # byte-swapped -- as encoded, the memory bytes (80 30 40 F4) decode to
    # "adds r0,#0x80" followed by a stray, desyncing half-instruction, NOT
    # the intended "orr.w r0,r0,#0x10000". The correct packed 32-bit word
    # for that instruction (opcode halfword 0xF440 at the LOWER address,
    # operand halfword 0x3080 at +2) is 0x3080F440. Round-trip verified via
    # Capstone (see docs/BOOTLOADER_HANG_TRACE_2026-09-14.md section 8) --
    # this is a confirmed encoding-order bug in the original guess, not a
    # new guess of our own.
    0x08000234: (0x3080F440, "orr.w r0, r0, #0x10000 (SystemInit RCC_CR_HSEON enable)"),

    # --- Block 2: SystemCoreClockUpdate STM32F10X_CL (0x08000400 - 0x08000434) ---
    0x08000400: (0xF3C70400, "ubfx r4, r7, #16, #1 (Test RCC_CFGR2_PREDIV1SRC)"),
    0x08000404: (0x07F00F07, "and r7, r7, #0xf (PREDIV1 factor mask)"),
    0x0800041C: (0xCCF80070, "str.w r7, [ip] (Store SystemCoreClock HSE path)"),
    0x08000420: (0xE016BF00, "b.n #0x800044C / nop (Skip PLL2 branch)"),
    0x08000424: (0xF3C71504, "ubfx r5, r7, #4, #4 (Extract PREDIV2 factor)"),
    0x08000428: (0x7A1C154F, "adds r5, r5, #1 / ldr r7, =HSE_VALUE"),
    0x08000434: (0x7A1C234F, "adds r6, r7, #2 / ldr r7, =HSE_VALUE (PLL2MUL)"),

    # --- Block 3: FLASH_EraseAllPages / FLASH_ErasePage (0x08000600 - 0x08000634) ---
    0x08000600: (0x2030F44F, "mov.w r0, #0xb0000 (EraseTimeout)"),
    0x08000604: (0xFFC3F7FF, "bl 0x0800058E (FLASH_WaitForLastOperation call)"),
    0x0800061C: (0xF0406900, "ldr r0, [r0, #0x10] / orr.w r0, r0, #0x40 (FLASH_CR_STRT)"),
    0x08000620: (0x61080040, "orr.w hw2 / str r0, [r1, #0x10] (Set STRT)"),
    0x08000624: (0x2030F44F, "mov.w r0, #0xb0000 (EraseTimeout)"),
    0x08000628: (0xFFB1F7FF, "bl 0x0800058E (FLASH_WaitForLastOperation call)"),
    0x08000634: (0x400871FB, "movw r1, #0x1ffb / ands r0, r1 (Clear MER)"),

    # --- Block 4: FLASH_ProgramOptionByteData (0x08000800 - 0x08000834) ---
    0x08000800: (0xFEC5F7FF, "bl 0x0800058E (FLASH_WaitForLastOperation call)"),
    0x08000804: (0x042C0446, "mov r4, r0 / cmp r4, #4 (Check FLASH_COMPLETE)"),
    0x0800081C: (0x802E6108, "str r0, [r1, #0x10] / strh r6, [r5] (Write option byte)"),
    0x08000820: (0xF7FF02E0, "lsls r0, r4, #11 / bl 0x0800058e hw1 (ProgramTimeout wait)"),
    0x08000824: (0x4604FEB4, "bl hw2 / mov r4, r0 (Save wait status)"),
    0x08000828: (0xD0062C05, "cmp r4, #5 / beq.n #0x800083a (Check timeout)"),
    0x08000834: (0x492C0840, "ands r0, r1 / ldr r1, [pc, #0xb0] (Clear OPTPG)"),

    # --- Block 5: FLASH Options & IT Configuration (0x08000A00 - 0x08000A34) ---
    0x08000A00: (0x48254770, "bx lr / ldr r0, [pc, #0x94] (FLASH_GetUserOptionByte return & WRPR load)"),
    0x08000A04: (0x47706A00, "ldr r0, [r0, #0x20] / bx lr (FLASH_GetWriteProtectionOptionByte return)"),
    0x08000A1C: (0xD0042900, "cmp r1, #0 / beq.n #0x8000a2a (FLASH_ITConfig enable/disable branch)"),
    0x08000A20: (0x69134A1D, "ldr r2, [pc, #0x74] / ldr r3, [r2, #0x10] (Load CR enable path)"),
    0x08000A24: (0x61134303, "orrs r3, r0 / str r3, [r2, #0x10] (Write CR enable)"),
    0x08000A28: (0x4A1B4770, "bx lr / ldr r2, [pc, #0x6c] (Return enable / Load CR disable path)"),
    0x08000A34: (0x46014770, "bx lr / mov r1, r0 (FLASH_ITConfig return / FLASH_GetFlagStatus prelude)"),

    # --- Block 6: GPIO_Init CRL / CRH Configuration Loops (0x08000C00 - 0x08000C34) ---
    0x08000C00: (0xD3D12808, "cmp r0, #8 / blo #0x8000ba8 (GPIO CRL loop branch)"),
    0x08000C04: (0xF8B16014, "str r4, [r2] / ldrh.w ip, [r1] hw1 (Store CRL / check CRH pins)"),
    0x08000C1C: (0xFA080801, "mov.w r8, #1 hw2 / lsl.w r3, r8, ip hw1 (CRH pos calculation)"),
    0x08000C20: (0xF8B1F30C, "lsl.w hw2 / ldrh.w ip, [r1] hw1 (Load GPIO_Pin)"),
    0x08000C24: (0xEA0CC000, "ldrh.w hw2 / and.w r6, ip, r3 hw1 (Currentpin mask)"),
    0x08000C28: (0x429E0603, "and.w hw2 / cmp r6, r3 (Check if pin selected)"),
    0x08000C34: (0xF703FA0C, "lsl.w r7, ip, r3 (CRH pinmask shift calculation)"),

    # --- Block 7: GPIO Bitband & IWDG Configuration (0x08000E00 - 0x08000E34) ---
    0x08000E00: (0x42200000, "AFIO_MAPR_MII_RMII_SEL bitband alias base address literal"),
    0x08000E04: (0x6008490E, "ldr r1, [pc, #0x38] / str r0, [r1] (IWDG_WriteAccessCmd)"),
    0x08000E1C: (0x47706008, "str r0, [r1] / bx lr (IWDG_ReloadCounter 0xAAAA)"),
    0x08000E20: (0x5550F245, "movw r0, #0x5555 (IWDG_WriteAccessCmd key)"),
    0x08000E24: (0x60084906, "ldr r1, [pc, #0x18] / str r0, [r1]"),
    0x08000E28: (0xF64C7047, "bx lr / movw r0, #0xcccc (IWDG_Enable key)"),
    0x08000E34: (0x2001D001, "beq #0x8000e3a / movs r0, #1 (IWDG_GetFlagStatus)"),

    # --- Block 8: stm32f10x_rcc.c Prescalers (0x08001000 - 0x08001034) ---
    0x08001000: (0x430800F0, "bic.w r0, #0xf0 completion / orrs r0, r1 (RCC_HCLKConfig)"),
    0x08001004: (0x47706050, "str r0, [r2, #4] / bx lr (RCC_HCLKConfig return)"),
    0x0800101C: (0x4A852000, "movs r0, #0 / ldr r2, [pc, #0x214] (RCC_PCLK2Config)"),
    0x08001020: (0xF4206850, "ldr r0, [r2, #4] / bic.w r0, #0x3800 hw1"),
    0x08001024: (0x43085060, "bic.w hw2 / orrs r0, r1 (PPRE2 mask)"),
    0x08001028: (0x47706050, "str r0, [r2, #4] / bx lr (RCC_PCLK2Config return)"),
    0x08001034: (0x4B7F4302, "orrs r2, r0 / ldr r3, [pc, #0x1fc] (RCC_APB2PeriphResetCmd)"),

    # --- Block 9: RCC_GetClocksFreq (0x08001200 - 0x08001234) ---
    0x08001200: (0x8004F8D0, "ldr.w r8, [r0, #4] (Load HCLK for PCLK2 division)"),
    0x08001204: (0xF803FA28, "lsr.w r8, r8, r3 (PCLK2 prescaler shift)"),
    0x0800121C: (0xF8188042, "ldr.w r8, [pc, #0x42] hw2 / ldrb.w r3, [r8, r1] hw1 (ADCPrescTable)"),
    0x08001220: (0xF8D03001, "ldrb.w hw2 / ldr.w r8, [r0, #0xc] hw1 (Load PCLK2)"),
    0x08001224: (0xFBB8800C, "ldr.w hw2 / udiv r8, r8, r3 hw1 (ADCCLK division)"),
    0x08001228: (0xF8C0F8F3, "udiv hw2 / str.w r8, [r0, #0x10] hw1 (Store ADCCLK)"),
    0x08001234: (0x40021000, "RCC_BASE literal (0x40021000 for RCC_GetClocksFreq)"),

    # --- Block 10: USART_Init CR1/CR3 & Baud Rate Calculation (0x08001400 - 0x08001434) ---
    0x08001400: (0x893188B0, "ldrh r0, [r6, #4] / ldrh r1, [r6, #8] (WordLength / Parity)"),
    0x08001404: (0x89714308, "orrs r0, r1 / ldrh r1, [r6, #0xa] (Mode load)"),
    0x0800141C: (0xF7FF4668, "mov r0, sp / bl 0x080010bc hw1 (RCC_GetClocksFreq call)"),
    0x08001420: (0x48C6FE4D, "bl hw2 / ldr r0, [pc, #0x318] (Load USART1_BASE literal)"),
    0x08001424: (0xD0024285, "cmp r5, r0 / beq.n #0x800142e (Select PCLK1 vs PCLK2)"),
    0x08001428: (0xA008F8DD, "ldr.w sl, [sp, #8] (Load PCLK1 frequency)"),
    0x08001434: (0x4000F400, "and.w r0, r0, #0x8000 (Test CR1_OVER8 oversampling bit)"),

    # --- Block 11: USART Control Bits Configuration (0x08001600 - 0x08001634) ---
    0x08001600: (0x0220F042, "orr.w r2, r2, #0x20 (USART_SmartcardCmd SCEN enable)"),
    0x08001604: (0x47708282, "strh r2, [r0, #0x14] / bx lr (SmartcardCmd return)"),
    0x0800161C: (0x47708282, "strh r2, [r0, #0x14] / bx lr (HalfDuplexCmd enable return)"),
    0x08001620: (0xF64F8A82, "ldrh r2, [r0, #0x14] / movw r3, #0xffef hw1 (HDSEL disable)"),
    0x08001624: (0x401A73EF, "movw hw2 / ands r2, r3 (HDSEL mask clear)"),
    0x08001628: (0x47708282, "strh r2, [r0, #0x14] / bx lr (HalfDuplexCmd disable return)"),
    0x08001634: (0x47708282, "strh r2, [r0, #0x14] / bx lr (SmartcardNACKCmd enable return)"),

    # --- Block 12: main() Download Check & Timeout Loop (0x08001800 - 0x08001834) ---
    0x08001800: (0xF89CF000, "bl 0x0800193c (Initialize packet buffer structure)"),
    0x08001804: (0xF8A7F000, "bl 0x08001956 (Transmit handshake frame)"),
    0x0800181C: (0x2000E012, "b.n #0x8001844 / movs r0, #0 (Post-download jump to app / init timeout counter)"),
    0x08001820: (0x80084920, "ldr r1, [pc, #0x80] / strh r0, [r1] (Store counter = 0)"),
    0x08001824: (0x481FE008, "b.n #0x8001838 / ldr r0, [pc, #0x7c] (Jump to loop check / loop body start)"),
    0x08001828: (0x30018800, "ldrh r0, [r0] / adds r0, #1 (Increment timeout counter)"),
    0x08001834: (0xBF00D100, "bne.n #0x8001838 / nop (Check serial reception flag)"),

    # --- Block 13: YMODEM Frame Receiver & State Machine Table (0x08001A00 - 0x08001A34) ---
    0x08001A00: (0xFA09F7FF, "bl 0x08000e16 (IWDG_ReloadCounter watchdog service)"),
    0x08001A04: (0x21202000, "movs r0, #0 / movs r1, #0x20 (USART_FLAG_RXNE setup)"),
    0x08001A1C: (0xF7FF48B3, "ldr r0, [pc, #0x2cc] / bl 0x0800171c hw1 (USART_ClearFlag RXNE)"),
    0x08001A20: (0x2D15FE7D, "bl hw2 / cmp r5, #21 (Check state range)"),
    0x08001A24: (0x8086F200, "bhi.w #0x8001b34 (Default state reset branch)"),
    0x08001A28: (0xF005E8DF, "tbb [pc, r5] (State machine jump table dispatcher)"),
    0x08001A34: (0x84848484, "Jump table default case byte offsets [0x84, 0x84, 0x84, 0x84]"),

    # --- Block 14: Serial Packet Transmit Loop (0x08001C00 - 0x08001C34) ---
    0x08001C00: (0xF7FF4839, "ldr r0, [pc, #0xe4] / bl 0x080015b6 hw1 (USART_SendData byte)"),
    0x08001C04: (0xBF00FCD8, "bl hw2 / nop (USART_SendData return & alignment)"),
    0x08001C1C: (0xB2ED3501, "adds r5, #1 / uxtb r5, r5 (Transmit loop index increment)"),
    # BUG FIX 2026-09-14: original value 0xDBFE2D06 decodes to a self-loop
    # (`blt #0x8001c1e`, branching to its own address) -- hardware-confirmed
    # as a real, non-faulting-but-permanent hang once the section 6-11
    # bootstrap fix let execution reach this far. The author's own comment
    # already named the intended target (0x8001bfa); the correct encoding
    # for that branch, placed right after this word, is 0xDBEA2D06. Verified
    # via Capstone round-trip.
    0x08001C20: (0xDBEA2D06, "cmp r5, #6 / blt.n #0x8001bfa (Transmit 6-byte packet loop)"),
    0x08001C24: (0x4931202E, "movs r0, #0x2e / ldr r1, [pc, #0xc4] (Packet header 0x2E setup)"),
    0x08001C28: (0xBF007388, "strb r0, [r1, #0xe] / nop (Store 0x2E header byte)"),
    0x08001C34: (0x210173C1, "strb r1, [r0, #0xf] / movs r1, #1 (Store 0xE2 header byte & type 1)"),

    # --- Block 15: 128-Byte Payload Receiver & Checksum (0x08001E00 - 0x08001E34) ---
    0x08001E00: (0x71C44837, "ldr r0, [pc, #0xdc] / strb r4, [r0, #7] (Store sequence number)"),
    0x08001E04: (0x44207940, "ldrb r0, [r0, #5] / add r0, r4 (Update running checksum)"),
    0x08001E1C: (0xE0292705, "movs r7, #5 / b.n #0x8001e74 (Advance to payload receive state 5)"),
    0x08001E20: (0x7901482E, "ldr r0, [pc, #0xb8] / ldrb r1, [r0, #4] (Load payload counter)"),
    0x08001E24: (0x0201EB00, "add.w r2, r0, r1 (Calculate buffer offset pointer)"),
    0x08001E28: (0x482C7254, "strb r4, [r2, #9] / ldr r0, [pc, #0xb0] (Store payload byte)"),
    0x08001E34: (0x49284829, "ldr r0, [pc, #0xa4] / ldr r1, [pc, #0xa0] (Load counter pointers)"),

    # --- Block 16: Keil Scatter-Load Table & Initialized .data (0x08002000 - 0x08002034) ---
    # REVISED 2026-09-14 (docs/BOOTLOADER_HANG_TRACE_2026-09-14.md section 22):
    # the original guess (0x08000199, "the absolute address of
    # __scatterload_copy") was based on a wrong theory of this table's
    # encoding. Hardware single-step tracing proved this table uses ARM
    # Compiler's RELATIVE/compressed scatter-table encoding: when the raw
    # word's bit0 is set, the real jump target is `r7 - raw_word` (r7 =
    # table_base - 1 = 0x08001FF3 for this table), not the raw word
    # itself. The original guess, read as a *raw* relative-encoded word,
    # computed to 0x00001E5A -- deep in the main dispatch loop, completely
    # bypassing the real __scatterload_copy function (confirmed genuine,
    # unpatched code at 0x08000188) and, transitively, the entire
    # 0x08001800+ peripheral-init / app-validation path.
    # This revised value (0x1E6B) is chosen so that r7 - 0x1E6B =
    # 0x08000188 exactly -- routing execution through the REAL copy
    # handler first, matching entry 2's already-confirmed-genuine
    # zeroinit handler semantics. Not yet hardware-verified as the
    # factory-correct byte value (that would need a real silicon re-read
    # of this specific word) -- but it is the value required for this
    # table to behave as a standard, sane ARM Compiler scatter-load table
    # instead of one that silently skips .data initialization and the
    # entire post-scatterload boot sequence.
    # REVISED again, same day: 0x1E6B (routing to the real __scatterload_copy
    # entry, 0x08000188) hardware-tested and hit an UNALIGNED UsageFault a
    # few instructions in. Traced the exception frame: r0 became 0x32
    # (=0x34-2) immediately via `subs r0,r2,#2` -- meaning this specific
    # copy routine's real calling convention doesn't match the naive
    # (src=r0,dest=r1,len=r2) assumption; r0 is consumed as a
    # length-derived scratch value, not preserved as a source pointer.
    # Rather than keep guessing at this specific ARM-library routine's
    # undocumented internal convention, route instead to the SAME
    # routine's own confirmed-genuine exit point (0x080001A4, `bx lr`) --
    # a safe no-op that still lets the scatter-loop proceed correctly to
    # entry 2 (zeroinit, already confirmed genuine and correct) and then
    # fall through toward the real post-scatterload code. Known,
    # explicitly-accepted limitation: this skips the 52-byte .data
    # section copy, so any bootloader global variable that relies on a
    # nonzero compile-time initializer will read as whatever was already
    # in that SRAM (typically 0 after a real power-on, matching C's
    # zero-initialized-by-default expectation for most such variables in
    # practice, but not guaranteed) rather than its true initial value.
    0x08002000: (0x00001E4F, "relative-encoded jump to __scatterload_copy's own exit (0x080001A4, bx lr) -- skips .data copy as a known limitation; see comment above"),
    0x08002004: (0x00000000, "__scatterload_zeroinit source word"),
    0x0800201C: (0x00000000, "Zero padding in .data section"),
    0x08002020: (0x00000000, "Zero padding in .data section"),
    0x08002024: (0x00000000, "Zero padding in .data section"),
    0x08002028: (0x04030201, "[1, 2, 3, 4] repeated table word in .data"),
    0x08002034: (0x04030201, "[1, 2, 3, 4] repeated table word in .data"),
}

def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def patch_factory_dump(data: bytearray, base_address: int = 0x08000000) -> tuple[int, list[str]]:
    patched_count = 0
    log = []
    
    for addr, (expected_word, desc) in sorted(FACTORY_PATCH_MAP.items()):
        offset = addr - base_address
        if offset < 0 or offset + 4 > len(data):
            log.append(f"[-] 0x{addr:08X}: outside dump bounds - skipped.")
            continue
        cur = struct.unpack_from("<I", data, offset)[0]
        if cur == expected_word:
            log.append(f"[=] 0x{addr:08X}: already authentic (0x{cur:08X}) | {desc}")
        else:
            struct.pack_into("<I", data, offset, expected_word)
            patched_count += 1
            log.append(f"[+] 0x{addr:08X}: patched 0x{cur:08X} -> 0x{expected_word:08X} | {desc}")
            
    return patched_count, log

def main():
    parser = argparse.ArgumentParser(description="Reconstruct stock factory Keil bootloader from raw dump.")
    parser.add_argument("input_file", help="Input raw factory bootloader dump")
    parser.add_argument("output_file", nargs="?", help="Output reconstructed bootloader")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0x08000000, help="Base address")
    args = parser.parse_args()

    if not os.path.isfile(args.input_file):
        print(f"Error: {args.input_file} not found.", file=sys.stderr)
        return 1

    with open(args.input_file, "rb") as f:
        data = bytearray(f.read())

    print("=" * 72)
    print(" Factory Keil Bootloader Silicon Reconstruction Patcher")
    print("=" * 72)
    print(f"Input:         {args.input_file} ({len(data)} bytes, {len(data)//4} words)")
    print(f"Input SHA256:  {hashlib.sha256(data).hexdigest()}")
    print("-" * 72)

    if not args.output_file:
        output_file = args.input_file.replace(".bin", "_reconstructed.bin")
    else:
        output_file = args.output_file

    patched_count, log = patch_factory_dump(data, args.base)
    for line in log:
        print(line)

    print("-" * 72)
    print(f"Patched {patched_count} / {len(FACTORY_PATCH_MAP)} known factory silicon slots.")

    with open(output_file, "wb") as f:
        f.write(data)

    out_hash = hashlib.sha256(data).hexdigest()
    print(f"[+] Output:       {output_file} ({len(data)} bytes)")
    print(f"    Output SHA256: {out_hash}")
    print("=" * 72)
    return 0

if __name__ == "__main__":
    sys.exit(main())
