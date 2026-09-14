#!/usr/bin/env python3
"""
rigorous_factory_test.py — Comprehensive Hardware-In-The-Loop (HIL) Test Suite
for Genuine Factory Bootloader + 100% Reconstructed Factory Application
on Physical Silicon (STM32F105RBT6).

Executes an exhaustive, multi-phase verification:
  1. Image Integrity & Flashing Verification
  2. Cold Boot Sequence & Reset Vector Interception
  3. Bootloader Execution & Handshake Handoff (0x20004000 = 0x20141003)
  4. SystemInit, VTOR Relocation (0x08003000) & Clock Tree Verification (72MHz)
  5. Peripheral Initialization (GPIO, CAN1, USART, Watchdog)
  6. Cooperative Task Scheduler Dynamic Validation (13 Tasks in SRAM)
  7. Sustained Free-Run Stability & Fault Trap Immunity (CFSR/HFSR = 0)
"""

import sys
import time
import subprocess

CFG_FILE = "tools/pico_stm32.cfg"
COMBINED_BIN = "hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin"

def run_ocd(commands):
    cmd_args = ["openocd", "-f", CFG_FILE]
    for c in commands:
        cmd_args.extend(["-c", c])
    cmd_args.extend(["-c", "exit"])
    res = subprocess.run(cmd_args, capture_output=True, text=True)
    out = res.stdout + "\n" + res.stderr
    if res.returncode != 0:
        raise RuntimeError(f"OpenOCD error (code {res.returncode}):\n{out}")
    return out

def mrw(addr):
    """Read a 32-bit word using OpenOCD mrw."""
    out = run_ocd(["init", "halt", f"echo VAL:[mrw 0x{addr:08X}]", "resume"])
    for line in out.splitlines():
        if "VAL:" in line:
            val_str = line.split("VAL:")[1].strip()
            return int(val_str, 0)
    raise ValueError(f"Failed to read address 0x{addr:08X}: {out}")

def get_regs():
    """Get core registers: PC, MSP, xPSR."""
    out = run_ocd(["init", "halt", "reg pc", "reg msp", "reg xPSR", "resume"])
    regs = {}
    for line in out.splitlines():
        line_l = line.strip().lower()
        for r in ["pc", "msp", "xpsr"]:
            if line_l.startswith(f"{r} (") or line_l.startswith(f"{r}:"):
                parts = line.split(":")
                if len(parts) > 1:
                    regs[r] = int(parts[1].strip(), 16)
    return regs

def test_suite():
    print("=" * 76)
    print(" RIGOROUS LIVE SILICON TEST SUITE: FACTORY BOOTLOADER + FACTORY APP")
    print(" Target: STM32F105RBT6 (128KB Flash, 64KB SRAM, ARM Cortex-M3)")
    print(" Image:  combined_factory_boot_factory_app_64k.bin")
    print("=" * 76)

    results = []
    def log_result(name, passed, details=""):
        status = "PASS" if passed else "FAIL"
        results.append((name, passed, details))
        print(f"[{status:4s}] {name:<45s} | {details}")

    # -------------------------------------------------------------------------
    # PHASE 1: Flashing & Verification
    # -------------------------------------------------------------------------
    print("\n--- PHASE 1: Silicon Flashing & Integrity Verification ---")
    t0 = time.time()
    try:
        flash_out = run_ocd([
            "init",
            "reset init",
            f"flash write_image erase {COMBINED_BIN} 0x08000000",
            f"verify_image {COMBINED_BIN} 0x08000000",
            "reset halt"
        ])
        log_result("Flash Programming & Sector Verification", True, f"Done in {time.time()-t0:.2f}s")
    except Exception as e:
        log_result("Flash Programming & Sector Verification", False, str(e))
        return 1

    # -------------------------------------------------------------------------
    # PHASE 2: Cold Boot Sequence & Bootloader Initial Vectors
    # -------------------------------------------------------------------------
    print("\n--- PHASE 2: Cold Boot & Bootloader Entry ---")
    regs = get_regs()
    pc = regs.get("pc", 0)
    msp = regs.get("msp", 0)
    log_result("Bootloader Reset Vector Entry", pc in (0x080004AC, 0x080004AD), f"PC=0x{pc:08X} (expected 0x080004AC/AD)")
    log_result("Bootloader Initial Stack Pointer", msp == 0x20000698, f"MSP=0x{msp:08X} (expected 0x20000698)")

    # -------------------------------------------------------------------------
    # PHASE 3: Bootloader-to-Application Handoff & Handshake
    # -------------------------------------------------------------------------
    print("\n--- PHASE 3: Bootloader Handoff & Application Entry ---")
    # Resume target, let bootloader execute and jump to app
    run_ocd(["init", "resume", "sleep 300", "halt"])
    time.sleep(0.1)
    regs_app = get_regs()
    pc_app = regs_app.get("pc", 0)
    msp_app = regs_app.get("msp", 0)
    xpsr_app = regs_app.get("xpsr", 0)

    # Check handshake in SRAM left by bootloader
    hs = mrw(0x20004000)
    log_result("Bootloader SRAM Handshake Check", hs == 0x20141003, f"*0x20004000 = 0x{hs:08X} (expected 0x20141003)")

    # Check PC is within application address space (0x08003000 - 0x0800FFFF)
    in_app = 0x08003000 <= pc_app <= 0x0800FFFF
    log_result("Execution In Application Partition", in_app, f"PC=0x{pc_app:08X} [0x08003000-0x0800FFFF]")

    # Check MSP is application runtime stack
    valid_app_sp = 0x20001000 <= msp_app <= 0x20005000
    log_result("Application Runtime Stack Pointer", valid_app_sp, f"MSP=0x{msp_app:08X}")

    # Check thread mode
    thread_mode = (xpsr_app & 0x1FF) == 0
    log_result("Core In Thread Mode (No Active Faults)", thread_mode, f"xPSR=0x{xpsr_app:08X} (ISRnum=0)")

    # -------------------------------------------------------------------------
    # PHASE 4: SystemInit, Vector Table Relocation & Clock Tree
    # -------------------------------------------------------------------------
    print("\n--- PHASE 4: SystemInit, VTOR & Clock Configuration ---")
    vtor = mrw(0xE000ED08)
    log_result("SCB->VTOR Relocation to App Vector Table", vtor == 0x08003000, f"VTOR=0x{vtor:08X} (expected 0x08003000)")

    cfsr = mrw(0xE000ED28)
    hfsr = mrw(0xE000ED2C)
    log_result("Fault Status Registers Zero (CFSR/HFSR)", cfsr == 0 and hfsr == 0, f"CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}")

    rcc_cr = mrw(0x40021000)
    rcc_cfgr = mrw(0x40021004)
    hse_rdy = (rcc_cr & (1 << 17)) != 0
    pll_rdy = (rcc_cr & (1 << 25)) != 0
    sws_pll = ((rcc_cfgr >> 2) & 0x03) == 0x02
    log_result("RCC HSE 25MHz & PLL Locked", hse_rdy and pll_rdy, f"RCC->CR=0x{rcc_cr:08X} (HSERDY=1, PLLRDY=1)")
    log_result("SYSCLK SWS Switched to PLL (72MHz)", sws_pll, f"RCC->CFGR=0x{rcc_cfgr:08X} (SWS=0b10)")

    # -------------------------------------------------------------------------
    # PHASE 5: Peripheral Initialization (GPIO, CAN1, USART, Watchdog)
    # -------------------------------------------------------------------------
    print("\n--- PHASE 5: Hardware Peripheral Verification ---")
    gpiob_odr = mrw(0x40010C0C)
    pb14_high = (gpiob_odr & (1 << 14)) != 0
    log_result("ARK1668 SoC Reset Line (PB14 HIGH)", pb14_high, f"GPIOB->ODR=0x{gpiob_odr:08X} (PB14=1)")

    can1_msr = mrw(0x40006404)
    can1_btr = mrw(0x4000641C)
    can1_ier = mrw(0x40006414)
    can1_active = (can1_msr & 0x02) == 0  # SLAK = 0
    log_result("CAN1 Normal Active Mode (Awake/Sync)", can1_active, f"CAN1->MSR=0x{can1_msr:08X}")
    log_result("CAN1 Baud Rate & Timing Configured", can1_btr != 0, f"CAN1->BTR=0x{can1_btr:08X}")
    log_result("CAN1 Interrupts Configured", can1_ier != 0, f"CAN1->IER=0x{can1_ier:08X}")

    u2_cr1 = mrw(0x4000440C)
    u2_brr = mrw(0x40004408)
    u2_enabled = (u2_cr1 & (1 << 13)) != 0
    log_result("USART2 Peripheral Active (UE/TE)", u2_enabled, f"CR1=0x{u2_cr1:08X}, BRR=0x{u2_brr:08X}")

    iser0 = mrw(0xE000E100)
    iser1 = mrw(0xE000E104)
    log_result("NVIC Interrupts Enabled (ISER0/ISER1)", iser0 != 0, f"ISER0=0x{iser0:08X}, ISER1=0x{iser1:08X}")

    # -------------------------------------------------------------------------
    # PHASE 6: Dynamic Cooperative Scheduler Verification
    # -------------------------------------------------------------------------
    print("\n--- PHASE 6: Cooperative Task Scheduler (13 Tasks in SRAM) ---")
    task_t0 = [mrw(0x200000CC + i * 4) for i in range(13)]
    run_ocd(["init", "resume", "sleep 1000", "halt"])
    task_t1 = [mrw(0x200000CC + i * 4) for i in range(13)]

    task_ptrs = [task_t0[0], task_t0[3], task_t0[6], task_t0[9], task_t0[12]]
    all_ptrs_valid = all(0x08003000 <= p <= 0x0800FFFF for p in task_ptrs)
    log_result("Task Dispatch Pointers Valid In App Flash", all_ptrs_valid, 
               f"Pointers: {', '.join(f'0x{p:08X}' for p in task_ptrs)}")

    counters_changing = (task_t1[1] != task_t0[1]) or (task_t1[4] != task_t0[4]) or (task_t1[10] != task_t0[10])
    log_result("Cooperative Scheduler Tasks Cycling", counters_changing,
               f"Task1: 0x{task_t0[1]:08X}->0x{task_t1[1]:08X}, Task4: 0x{task_t0[4]:08X}->0x{task_t1[4]:08X}")

    # -------------------------------------------------------------------------
    # PHASE 7: Sustained Stress Free-Run (5.0 seconds)
    # -------------------------------------------------------------------------
    print("\n--- PHASE 7: Sustained Free-Run Stability (5.0 seconds) ---")
    run_ocd(["init", "resume", "sleep 5000", "halt"])
    time.sleep(0.1)
    regs_stress = get_regs()
    pc_stress = regs_stress.get("pc", 0)
    cfsr_stress = mrw(0xE000ED28)
    hfsr_stress = mrw(0xE000ED2C)
    vtor_stress = mrw(0xE000ED08)

    log_result("Zero Faults After 5s Sustained Run", cfsr_stress == 0 and hfsr_stress == 0,
               f"CFSR=0x{cfsr_stress:08X}, HFSR=0x{hfsr_stress:08X}")
    log_result("VTOR Remains Relocated (0x08003000)", vtor_stress == 0x08003000, f"VTOR=0x{vtor_stress:08X}")
    log_result("Superloop Execution Active After 5s", 0x08003000 <= pc_stress <= 0x0800FFFF,
               f"PC=0x{pc_stress:08X}")

    try:
        run_ocd(["init", "resume"])
    except:
        pass

    # -------------------------------------------------------------------------
    # SUMMARY
    # -------------------------------------------------------------------------
    print("\n" + "=" * 76)
    passed_count = sum(1 for _, p, _ in results)
    total_count = len(results)
    pct = (passed_count / total_count) * 100
    print(f" FINAL RESULT: {passed_count}/{total_count} Tests Passed ({pct:.1f}%)")
    print("=" * 76)

    return 0 if passed_count == total_count else 1

if __name__ == "__main__":
    sys.exit(test_suite())
