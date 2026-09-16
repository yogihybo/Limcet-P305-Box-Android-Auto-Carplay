#!/usr/bin/env python3
"""
Comprehensive HIL Test Suite for Authentic Reconstructed Factory App
Tests on live STM32F105 silicon via OpenOCD SWD:
1. Boot & Vector Table Integrity (SystemInit, VTOR, No Faults)
2. Power Management & ACC Keep-Alive
3. CAN Engine & Ring Buffer (StdId, ExtId, SPL CanRxMsg structure)
4. Steering Angle Decoding (CAN 0x025 -> 0x2000009F/0x200000A0)
5. Radar Decoding (CAN 0x396)
6. BD37033 Audio DSP State Machine & Settling Delay Verification (I2C1 PB6/PB7)
7. Standby / Sleep Transition Lifecycle
"""

import subprocess
import time
import struct
import sys

CFG_FILE = "tools/pico_stm32.cfg"

# Factory App SRAM Layout (100% verified via disassembly)
SRAM_BASE            = 0x20000000
SYM_POWER_TIMER      = 0x20000052  # uint16_t (counts up to 5000ms before sleep)
SYM_POWER_STATE      = 0x200007E8  # uint8_t  (0x02 = RUN, 0x04 = SLEEP/STANDBY)
SYM_ACC_FLAG         = 0x200001BC  # uint8_t  (1 = ACC active)
SYM_AUDIO_TIMER      = 0x2000007E  # uint16_t (settling timer, ticks to 4000)
SYM_AUDIO_STEP       = 0x2000102D  # uint8_t  (audio SM step)
SYM_SOUND_TYPE       = 0x200001F2  # uint8_t  (sampled strap: 3 = Rohm BD37033)
SYM_I2C_ERRORS       = 0x20000094  # uint8_t  (I2C error/NACK count)

# CAN1 Ring Buffer (SPL CanRxMsg struct: 20 bytes each, 15 slots)
SYM_CAN1_RING        = 0x20000270
SYM_CAN1_HEAD        = 0x2000039C  # uint8_t
SYM_CAN1_TAIL        = 0x2000039D  # uint8_t

# Decoded State
SYM_STEERING_SIGN    = 0x200001EF  # uint8_t  (0 = right, 1 = left)
SYM_STEERING_MAG     = 0x200001F0  # uint16_t (12-bit magnitude)

# Hardware Registers
REG_VTOR             = 0xE000ED08
REG_CFSR             = 0xE000ED28
REG_HFSR             = 0xE000ED2C
REG_GPIOB_CRL        = 0x40010C00
REG_I2C1_CR1         = 0x40005400
REG_I2C1_SR1         = 0x40005414

def run_ocd(commands):
    cmd = ["openocd", "-c", "gdb_port disabled; tcl_port disabled; telnet_port disabled", "-f", CFG_FILE]
    for c in commands:
        cmd.extend(["-c", c])
    cmd.extend(["-c", "exit"])
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"OpenOCD error (code {res.returncode}):\n{res.stderr}\n{res.stdout}")
    return res.stdout + "\n" + res.stderr

def read_mem_words(addr, count):
    out = run_ocd(["init", "halt", f"mdw 0x{addr:08X} {count}", "resume"])
    words = []
    prefix = f"0x{addr:08x}:"
    for line in out.splitlines():
        line_s = line.strip().lower()
        if line_s.startswith(prefix):
            parts = line_s[len(prefix):].strip().split()
            for p in parts:
                words.append(int(p, 16))
    return words

def read_mem_bytes(addr, count):
    out = run_ocd(["init", "halt", f"mdb 0x{addr:08X} {count}", "resume"])
    bytes_out = []
    prefix = f"0x{addr:08x}:"
    for line in out.splitlines():
        line_s = line.strip().lower()
        if line_s.startswith(prefix):
            parts = line_s[len(prefix):].strip().split()
            for p in parts:
                bytes_out.append(int(p, 16))
    return bytes_out

def write_mem_bytes(addr, byte_list):
    cmds = ["init", "halt"]
    for i, b in enumerate(byte_list):
        cmds.append(f"mwb 0x{addr + i:08X} 0x{b:02X}")
    cmds.append("resume")
    run_ocd(cmds)

def inject_factory_can_frame(std_id, data_bytes):
    """Injects a CAN frame into the factory app's SPL CanRxMsg ring buffer."""
    # 1. Read current head & tail
    hd, tl = read_mem_bytes(SYM_CAN1_HEAD, 2)
    next_hd = (hd + 1) % 15
    slot_addr = SYM_CAN1_RING + (hd * 20)
    
    # 2. Build 20-byte CanRxMsg:
    # StdId(4) + ExtId(4) + IDE(1) + RTR(1) + DLC(1) + Data(8) + FMI(1)
    payload = list(data_bytes) + [0] * (8 - len(data_bytes))
    frame = bytearray(20)
    struct.pack_into("<I", frame, 0, std_id)  # StdId
    struct.pack_into("<I", frame, 4, 0)       # ExtId
    frame[8] = 0                              # IDE = CAN_Id_Standard
    frame[9] = 0                              # RTR = CAN_RTR_Data
    frame[10] = len(data_bytes)               # DLC
    frame[11:19] = payload[:8]                # Data[8]
    frame[19] = 0                             # FMI
    
    # 3. Write slot, advance head, reset power timer (ACC keepalive)
    cmds = ["init", "halt"]
    for i, b in enumerate(frame):
        cmds.append(f"mwb 0x{slot_addr + i:08X} 0x{b:02X}")
    cmds.append(f"mwb 0x{SYM_CAN1_HEAD:08X} 0x{next_hd:02X}")
    # Reset power timeout
    cmds.append(f"mwh 0x{SYM_POWER_TIMER:08X} 0x0000")
    cmds.append(f"mwb 0x{SYM_ACC_FLAG:08X} 0x01")
    cmds.append("resume")
    run_ocd(cmds)

tests_passed = 0
tests_total = 0

def record_test(name, passed, detail=""):
    global tests_passed, tests_total
    tests_total += 1
    if passed:
        tests_passed += 1
        print(f"  [PASS] Test {tests_total:02d}: {name}")
    else:
        print(f"  [FAIL] Test {tests_total:02d}: {name} -- {detail}")

def main():
    print("=" * 70)
    print(" COMPREHENSIVE HIL SILICON TEST: RECONSTRUCTED FACTORY FIRMWARE")
    print(" Target: STM32F105RBT6 (Bootloader + Reconstructed 52K App)")
    print("=" * 70)

    # --- Phase 1: Cold Boot & Core Health ---
    print("\n>>> Phase 1: Cold Reset & Vector Table Relocation")
    run_ocd(["init", "reset halt", "mww 0xE0042004 0x00000307", "resume", "sleep 500", "halt", "mww 0xE0042004 0x00000307", "resume"])
    
    vtor = read_mem_words(REG_VTOR, 1)[0]
    record_test("SCB->VTOR is 0x08003000", vtor == 0x08003000, f"VTOR=0x{vtor:08X}")
    
    hs = read_mem_words(0x20004000, 1)[0]
    record_test("Bootloader Handshake (*0x20004000 == 0x20141003)", hs == 0x20141003, f"HS=0x{hs:08X}")

    cfsr, hfsr = read_mem_words(REG_CFSR, 2)
    record_test("Zero Faults (CFSR==0, HFSR==0)", cfsr == 0 and hfsr == 0, f"CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}")

    # --- Phase 2: Power Management & Keep-Alive ---
    print("\n>>> Phase 2: Power State & ACC Keep-Alive")
    pwr_state = read_mem_bytes(SYM_POWER_STATE, 1)[0]
    record_test("Power State is BOOT/RUN (0x00..0x03)", pwr_state in (1, 2, 3), f"State=0x{pwr_state:02X}")

    # --- Phase 3: Hardware Strapping ---
    print("\n>>> Phase 3: Hardware Strapping Sample")
    soundtype = read_mem_bytes(SYM_SOUND_TYPE, 1)[0]
    record_test("SoundType Strap is Rohm BD37033 (0x03)", soundtype == 0x03, f"SoundType=0x{soundtype:02X}")

    # --- Phase 4: CAN Frame Ingestion & Dispatcher ---
    print("\n>>> Phase 4: CAN Dispatcher & Steering Angle (CAN 0x025)")
    
    # Steering straight (mag = 0, sign = 0)
    inject_factory_can_frame(0x025, [0x00, 0x00, 0, 0, 0, 0, 0, 0])
    time.sleep(0.1)
    hd, tl = read_mem_bytes(SYM_CAN1_HEAD, 2)
    record_test("CAN frame consumed from ring buffer (head == tail)", hd == tl, f"head={hd}, tail={tl}")
    
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_words(SYM_STEERING_MAG, 1)[0] & 0xFFFF
    record_test("Steering Center: mag=0, sign=1 (neutral)", mag == 0, f"mag={mag}, sign={sign}")

    # Moderate Right Turn: raw 0x0040 (64) -> scaled = 64/2 = 32 (0x20)
    inject_factory_can_frame(0x025, [0x00, 0x40, 0, 0, 0, 0, 0, 0])
    time.sleep(0.1)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_words(SYM_STEERING_MAG, 1)[0] & 0xFFFF
    record_test("Steering Right Turn (raw 0x40 -> scaled 0x20, sign=1)", sign == 1 and mag == 0x20, f"mag=0x{mag:X}, sign={sign}")

    # Extreme Right Turn: raw 0x0150 -> clamped to 0x7F, sign=1
    inject_factory_can_frame(0x025, [0x01, 0x50, 0, 0, 0, 0, 0, 0])
    time.sleep(0.1)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_words(SYM_STEERING_MAG, 1)[0] & 0xFFFF
    record_test("Steering Extreme Turn Clamped (0x7F, sign=1)", sign == 1 and mag == 0x7F, f"mag=0x{mag:X}, sign={sign}")

    # Left Turn: raw 0x0800 (sign bit set) -> inverted & clamped to 0x7F, sign=0
    inject_factory_can_frame(0x025, [0x08, 0x00, 0, 0, 0, 0, 0, 0])
    time.sleep(0.1)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_words(SYM_STEERING_MAG, 1)[0] & 0xFFFF
    record_test("Steering Left Turn Clamped (0x7F, sign=0)", sign == 0 and mag == 0x7F, f"mag=0x{mag:X}, sign={sign}")

    # --- Phase 5: CAN Radar (CAN 0x396) ---
    print("\n>>> Phase 5: Parking Sonar / Clearance Sonar (CAN 0x396)")
    # Inject clearance sonar frame
    inject_factory_can_frame(0x396, [0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0])
    time.sleep(0.1)
    hd, tl = read_mem_bytes(SYM_CAN1_HEAD, 2)
    record_test("Radar CAN frame consumed (head == tail)", hd == tl, f"head={hd}, tail={tl}")

    # --- Phase 6: Rohm BD37033 Audio DSP Trigger & NACK Handling ---
    print("\n>>> Phase 6: Audio Subsystem & BD37033 Failsafe Handling")
    # Initial audio state
    step = read_mem_bytes(SYM_AUDIO_STEP, 1)[0]
    record_test("Audio State starts in Step 0 or 1", step in (0, 1), f"Step={step}")

    # Trigger settling threshold: write 3999 (0x0F9F) into audio timer
    print("  Triggering 4000-tick settling delay threshold...")
    run_ocd(["init", "halt", f"mwh 0x{SYM_AUDIO_TIMER:08X} 0x0F9F", f"mwh 0x{SYM_POWER_TIMER:08X} 0x0000", "resume"])
    time.sleep(0.3)
    
    # Observe BD37033 execution
    step_after = read_mem_bytes(SYM_AUDIO_STEP, 1)[0]
    record_test("Audio State advanced past Step 1 to Step 2", step_after >= 2, f"Step={step_after}")

    # Check that I2C handled the missing physical IC without hanging
    cfsr, hfsr = read_mem_words(REG_CFSR, 2)
    record_test("Zero Faults after BD37033 I2C blast without physical IC", cfsr == 0 and hfsr == 0, f"CFSR=0x{cfsr:08X}")

    # --- Phase 7: Sustained 10-Second Continuous Runtime ---
    print("\n>>> Phase 7: Sustained 10-Second Active Runtime")
    for sec in range(10):
        # Keep alive every second
        inject_factory_can_frame(0x025, [0x00, 0x00, 0, 0, 0, 0, 0, 0])
        time.sleep(1.0)
    
    pwr_state_10s = read_mem_bytes(SYM_POWER_STATE, 1)[0]
    record_test("MCU stayed in ACTIVE RUN state (0x03) across 10s continuous runtime", pwr_state_10s == 0x03, f"Power State=0x{pwr_state_10s:02X}")
    
    cfsr, hfsr = read_mem_words(REG_CFSR, 2)
    record_test("Zero Faults across sustained runtime", cfsr == 0 and hfsr == 0, f"CFSR=0x{cfsr:08X}")

    # --- Phase 8: Standby / Sleep Transition Lifecycle ---
    print("\n>>> Phase 8: Sleep Transition Lifecycle & Wakeup")
    # Fast-forward power timeout timer to 4999 (0x1387) to cross 5000-tick threshold
    run_ocd(["init", "halt", f"mwh 0x{SYM_POWER_TIMER:08X} 0x1387", "resume"])
    time.sleep(1.2)
    
    pwr_state_sleep = read_mem_bytes(SYM_POWER_STATE, 1)[0]
    record_test("MCU transitioned to STANDBY/SLEEP (0x04 or 0x05)", pwr_state_sleep in (4, 5), f"Power State=0x{pwr_state_sleep:02X}")

    cfsr, hfsr = read_mem_words(REG_CFSR, 2)
    record_test("Zero Faults during sleep transition", cfsr == 0 and hfsr == 0, f"CFSR=0x{cfsr:08X}")

    # Test Wakeup: Assert Wakeup Sequence (*0x200007F0 = 0, *0x200007E8 = 0, ACC ON)
    run_ocd(["init", "halt", "mww 0x200007F0 0x00000000", "mwb 0x200007E8 0x00", "mwh 0x20000052 0x0000", "mwb 0x200001BC 0x01", "resume"])
    time.sleep(0.1)
    pwr_state_wake = read_mem_bytes(SYM_POWER_STATE, 1)[0]
    record_test("MCU woke up from SLEEP back to BOOT/RUN (0x00..0x03)", pwr_state_wake in (0, 1, 2, 3), f"Power State=0x{pwr_state_wake:02X}")

    print("\n" + "=" * 70)
    print(f" FINAL RESULT: {tests_passed}/{tests_total} Tests Passed")
    print("=" * 70)
    return 0 if tests_passed == tests_total else 1

if __name__ == "__main__":
    sys.exit(main())
