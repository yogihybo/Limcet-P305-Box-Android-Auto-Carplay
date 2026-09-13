#!/usr/bin/env python3
"""
test_factory_restore.py — Validates the reconstructed, patched factory firmware
by flashing it to the attached physical STM32F105 development board and inspecting
silicon state via OpenOCD.
"""

import sys
import time
import subprocess

CFG_FILE = "tools/pico_stm32.cfg"
RESTORE_BIN = "hardware/MCU/live_dumps/factory_full_64k_patched.bin"

# Memory & Hardware Registers
REG_CFSR        = 0xE000ED28
REG_HFSR        = 0xE000ED2C
REG_RCC_CR      = 0x40021000
REG_RCC_CFGR    = 0x40021004
REG_GPIOB_CRL   = 0x40010C00
REG_GPIOB_CRH   = 0x40010C04
REG_GPIOB_ODR   = 0x40010C0C
REG_USART2_SR   = 0x40004400
REG_USART2_BRR  = 0x40004408
REG_USART2_CR1  = 0x4000440C
REG_CAN1_MSR    = 0x40006404
REG_CAN1_BTR    = 0x4000641C

def run_ocd_commands(cmds):
    cmd_args = ["openocd", "-f", CFG_FILE]
    for c in cmds:
        cmd_args.extend(["-c", c])
    cmd_args.extend(["-c", "exit"])
    res = subprocess.run(cmd_args, capture_output=True, text=True)
    output = res.stdout + "\n" + res.stderr
    if res.returncode != 0:
        raise RuntimeError(f"OpenOCD failed (code {res.returncode}):\n{output}")
    return output

def read_mem_words(addr, count):
    out = run_ocd_commands(["init", "halt", f"mdw 0x{addr:08X} {count}", "resume"])
    words = []
    prefix = f"0x{addr:08x}:"
    for line in out.splitlines():
        line_s = line.strip().lower()
        if line_s.startswith(prefix):
            parts = line_s[len(prefix):].strip().split()
            for p in parts:
                words.append(int(p, 16))
    return words

def get_target_reg(reg_name):
    out = run_ocd_commands(["init", "halt", f"reg {reg_name}", "resume"])
    for line in out.splitlines():
        line_s = line.strip().lower()
        if line_s.startswith(f"{reg_name.lower()} ("):
            parts = line_s.split(":")
            if len(parts) > 1:
                return int(parts[1].strip(), 16)
    return None

def main():
    print("=" * 70)
    print(" Factory Firmware Restore — Live Hardware Verification Suite")
    print("=" * 70)

    # 1. Program the full patched factory image
    print(f"\n>>> 1. In-System Flashing: {RESTORE_BIN} (64 KB)")
    t0 = time.time()
    flash_out = run_ocd_commands([
        "init",
        "reset init",
        f"flash write_image erase {RESTORE_BIN} 0x08000000",
        f"verify_image {RESTORE_BIN} 0x08000000",
        "reset run"
    ])
    print(f"    Flashing and verification completed in {time.time() - t0:.2f}s.")
    print("    Verified 65536 bytes identical to binary image.")

    print("\n>>> 2. Allowing factory firmware to boot (1.0s)...")
    time.sleep(1.0)

    passed = 0
    total = 0

    def assert_test(cond, desc, details=""):
        nonlocal passed, total
        total += 1
        if cond:
            passed += 1
            print(f" [PASS] Test {total:02d}: {desc}")
        else:
            print(f" [FAIL] Test {total:02d}: {desc} -- {details}")

    # TEST 1: Core Health & System Faults
    print("\n>>> 3. Core Health & Fault Registers")
    faults = read_mem_words(REG_CFSR, 2)
    cfsr = faults[0] if faults else 0xFFFFFFFF
    hfsr = faults[1] if len(faults) > 1 else 0xFFFFFFFF
    assert_test(cfsr == 0, "CFSR Fault Register is Zero", f"CFSR = 0x{cfsr:08X}")
    assert_test(hfsr == 0, "HFSR HardFault Register is Zero", f"HFSR = 0x{hfsr:08X}")

    # TEST 2: Bootloader to Application Handoff
    print("\n>>> 4. Bootloader to Factory Application Execution")
    pc = get_target_reg("pc")
    msp = get_target_reg("msp")
    print(f"    Live Target State: PC=0x{pc:08X}, MSP=0x{msp:08X}")
    in_app = (pc is not None and 0x08003000 <= pc <= 0x0800FFFF)
    valid_sp = (msp is not None and 0x20000000 <= msp <= 0x20005000)
    assert_test(in_app, f"CPU Running in Application Space (PC=0x{pc:08X})", f"PC=0x{pc:08X}")
    assert_test(valid_sp, f"Valid Main Stack Pointer (MSP=0x{msp:08X})", f"MSP=0x{msp:08X}")

    # TEST 3: Clock Tree
    print("\n>>> 5. Clock Tree & PLL Lock")
    rcc = read_mem_words(REG_RCC_CR, 2)
    cr = rcc[0] if len(rcc) > 0 else 0
    cfgr = rcc[1] if len(rcc) > 1 else 0
    hse_rdy = (cr & (1 << 17)) != 0
    pll_rdy = (cr & (1 << 25)) != 0
    sws_pll = ((cfgr >> 2) & 0x03) == 0x02
    assert_test(hse_rdy and pll_rdy, "HSE 25MHz & PLL Clock Locked", f"CR = 0x{cr:08X}")
    assert_test(sws_pll, "SYSCLK Driven by PLL (72 MHz)", f"CFGR = 0x{cfgr:08X}")

    # TEST 4: GPIOB Pin 14 — ARK1668 SoC Hardware Reset Release
    print("\n>>> 6. ARK1668 SoC Hardware Reset Pin (GPIOB Pin 14)")
    gpiob_odr = read_mem_words(REG_GPIOB_ODR, 1)[0]
    pb14_high = (gpiob_odr & (1 << 14)) != 0
    assert_test(pb14_high, "GPIOB Pin 14 is HIGH (ARK1668 SoC Released From Reset)", f"GPIOB->ODR = 0x{gpiob_odr:08X}")

    # TEST 5: USART2 (SoC Link) Configuration
    print("\n>>> 7. USART2 Hardware Link Configuration (PA2/PA3)")
    u2_cr1 = read_mem_words(REG_USART2_CR1, 1)[0]
    u2_brr = read_mem_words(REG_USART2_BRR, 1)[0]
    u2_enabled = (u2_cr1 & (1 << 13)) != 0 and (u2_cr1 & (1 << 3)) != 0
    baud_38400 = (u2_brr == 0x03A98)
    assert_test(u2_enabled, "USART2 Peripheral Enabled (UE/TE active)", f"CR1 = 0x{u2_cr1:08X}")
    assert_test(baud_38400, "USART2 Baud Rate = 38,400 baud (BRR=0x03A98 @ 36MHz APB1)", f"BRR = 0x{u2_brr:08X}")

    # TEST 6: CAN1 Subsystem
    print("\n>>> 8. CAN1 Controller State")
    can1_msr = read_mem_words(REG_CAN1_MSR, 1)[0]
    can1_btr = read_mem_words(REG_CAN1_BTR, 1)[0]
    can1_active = (can1_msr & 0x02) == 0  # Not in sleep
    can1_timing = (can1_btr == 0x00140008)
    assert_test(can1_active, "CAN1 Awake & Operating", f"CAN1->MSR = 0x{can1_msr:08X}")
    assert_test(can1_timing, "CAN1 500 kbps @ 75% Timing Configured", f"CAN1->BTR = 0x{can1_btr:08X}")

    # TEST 7: Sustained Execution & Watchdog Stability
    print("\n>>> 9. Sustained Hardware Execution Run (3.0s)")
    print("    Running continuously on silicon...")
    time.sleep(3.0)
    faults_end = read_mem_words(REG_CFSR, 2)
    cfsr_end = faults_end[0] if faults_end else 0xFFFFFFFF
    pc_end = get_target_reg("pc")
    assert_test(cfsr_end == 0, f"No Faults After Sustained Run (CFSR=0x{cfsr_end:08X})")
    assert_test(pc_end is not None and 0x08003000 <= pc_end <= 0x0800FFFF,
                f"Application Continuously Running (PC=0x{pc_end:08X})")

    print("\n" + "=" * 70)
    print(f" Factory Restore Verification: {passed}/{total} Tests Passed ({100.0 * passed / total:.1f}%)")
    print("=" * 70)

    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
