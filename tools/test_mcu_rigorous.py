#!/usr/bin/env python3
"""
Rigorous Hardware-In-The-Loop (HIL) Test Suite for STM32F105 Companion MCU Firmware
Communicates directly with the live STM32F105RBT6 test board via OpenOCD SWD.
"""

import sys
import time
import subprocess
import struct
import shutil
import re

CFG_FILE = "tools/pico_stm32.cfg"
ELF_FILE = "hardware/MCU/source/build/can_app.elf"

# ----------------------------------------------------------------------------
# Symbol resolution: every SRAM address below is derived from the CURRENTLY
# BUILT ELF (via `nm -S`), not hardcoded. Hardcoded addresses have broken
# repeatedly this project (three separate times in one session: a wrong
# manual guess, a two-byte confusion between adjacent .bss variables, and
# every single one shifting after any unrelated source edit added/removed a
# global -- .bss/.data layout is purely sequential, so ANY change anywhere
# in the firmware source can silently invalidate every hardcoded address in
# this file). Resolving from the real build eliminates that whole class of
# bug. Falls back to the last-known-good table below (documented as of the
# 2026-09-14 mic-mux-fix build) only if the toolchain's `nm` isn't available
# and the symbol truly can't be resolved -- with a loud warning, since a
# fallback address is exactly as fragile as the old hardcoded scheme.
_FALLBACK_SYMBOLS = {
    "g_active_mode": 0x20000000, "g_rear_radar": 0x20000001,
    "g_front_radar": 0x20000005, "g_vehicle_status_byte": 0x20000009,
    "s_power_state": 0x2000000A, "g_system_ticks_ms": 0x2000000C,
    "g_can_rx_ring": 0x20000010, "g_door_open": 0x20000177,
    "g_gear_field": 0x20000178, "g_steering_mag": 0x20000179,
    "g_steering_sign": 0x2000017A, "g_rx_tail": 0x2000017B,
    "g_rx_head": 0x2000017C, "g_rx_ring": 0x2000017D,
    "g_last_door_state": 0x200002B2, "g_last_lights_state": 0x200002B3,
    "g_last_reverse_state": 0x200002B4, "s_can_activity_flag": 0x200002D5,
    "s_timer_counter": 0x200002D6,
}

def _find_nm():
    for name in ("arm-none-eabi-nm",):
        p = shutil.which(name)
        if p:
            return p
    return None

def _resolve_symbols():
    """Returns {name: (address, size)} parsed from `nm -S` on the current
    ELF build. Falls back to _FALLBACK_SYMBOLS (size unknown -> 1) with a
    warning if the toolchain or ELF isn't available."""
    nm = _find_nm()
    import os
    if nm and os.path.isfile(ELF_FILE):
        out = subprocess.run([nm, "-S", ELF_FILE], capture_output=True, text=True)
        if out.returncode == 0:
            syms = {}
            for line in out.stdout.splitlines():
                m = re.match(r"^([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s+\S\s+(\S+)$", line.strip())
                if m:
                    addr, size, name = m.groups()
                    syms[name] = (int(addr, 16), int(size, 16))
            missing = [k for k in _FALLBACK_SYMBOLS if k not in syms]
            if not missing:
                return syms
            print(f"WARNING: nm resolved but missing symbols {missing}, using fallback for those", file=sys.stderr)
            for k in missing:
                syms[k] = (_FALLBACK_SYMBOLS[k], 1)
            return syms
    print(f"WARNING: could not resolve symbols via nm ({ELF_FILE!r}, toolchain={nm!r}) "
          f"-- using hardcoded fallback addresses. Run `make` in hardware/MCU/source/ "
          f"first, and ensure arm-none-eabi-nm is on PATH, for accurate addresses.",
          file=sys.stderr)
    return {k: (v, 1) for k, v in _FALLBACK_SYMBOLS.items()}

_SYMS = _resolve_symbols()

def _addr(name):
    return _SYMS[name][0]

def _size(name):
    return _SYMS[name][1]

def run_ocd_commands(cmds):
    """Executes a batch of OpenOCD commands and returns the stdout."""
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
    """Reads 32-bit words from target memory."""
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

def read_mem_bytes(addr, count):
    """Reads bytes from target memory."""
    out = run_ocd_commands(["init", "halt", f"mdb 0x{addr:08X} {count}", "resume"])
    bytes_out = []
    prefix = f"0x{addr:08x}:"
    for line in out.splitlines():
        line_s = line.strip().lower()
        if line_s.startswith(prefix):
            parts = line_s[len(prefix):].strip().split()
            for p in parts:
                bytes_out.append(int(p, 16))
    return bytes_out

# Memory Map Constants -- resolved from the current build via nm (see
# _resolve_symbols() above), not hardcoded. g_can_rx_ring's head/tail
# fields are struct members (not standalone symbols), computed as an
# offset from the struct's own real, resolved base address: CanRingBuffer
# = CanFrame frames[15] (20 bytes each = 300 = 0x12C) + head(1) + tail(1),
# per can_driver.h -- offsets 0x12C/0x12D are struct layout, not a
# separately-fragile address.
SYM_ACTIVE_MODE      = _addr("g_active_mode")
SYM_REAR_RADAR       = _addr("g_rear_radar")       # 4 bytes [RL, RML, RMR, RR]
SYM_FRONT_RADAR      = _addr("g_front_radar")      # 4 bytes [FL, FML, FMR, FR]
SYM_VEHICLE_STATUS   = _addr("g_vehicle_status_byte")  # 1 byte
SYM_POWER_STATE      = _addr("s_power_state")      # power_state_t; size is
                                                    # TOOLCHAIN-DEPENDENT (an
                                                    # enum with only 6 values
                                                    # may be sized 1 or 4
                                                    # bytes depending on the
                                                    # compiler) -- always
                                                    # read/write this one via
                                                    # the byte-sized helpers
                                                    # (read_mem_bytes/mwb),
                                                    # never mdw/mww, to stay
                                                    # correct either way.
SYM_SYSTEM_TICKS     = _addr("g_system_ticks_ms")  # 4 bytes uint32
SYM_CAN_RING         = _addr("g_can_rx_ring")      # CanRingBuffer
SYM_CAN_HEAD         = SYM_CAN_RING + 0x12C        # uint8_t (struct offset)
SYM_CAN_TAIL         = SYM_CAN_RING + 0x12D        # uint8_t (struct offset)
SYM_DOOR_OPEN        = _addr("g_door_open")        # uint8_t
SYM_GEAR_FIELD       = _addr("g_gear_field")       # uint8_t
SYM_STEERING_MAG     = _addr("g_steering_mag")     # uint8_t
SYM_STEERING_SIGN    = _addr("g_steering_sign")    # uint8_t
SYM_LAST_REVERSE     = _addr("g_last_reverse_state")  # uint8_t
SYM_LAST_LIGHTS      = _addr("g_last_lights_state")   # uint8_t
SYM_LAST_DOOR        = _addr("g_last_door_state")     # uint8_t
SYM_CAN_ACTIVITY     = _addr("s_can_activity_flag")   # uint8_t (power_manager.o)
SYM_TIMER_COUNTER    = _addr("s_timer_counter")       # uint16_t (power_manager.o)

# Hardware Peripherals
REG_CFSR             = 0xE000ED28
REG_HFSR             = 0xE000ED2C
REG_RCC_CR           = 0x40021000
REG_RCC_CFGR         = 0x40021004
REG_AFIO_MAPR        = 0x40010004
REG_CAN1_MSR         = 0x40006404
REG_CAN1_BTR         = 0x4000641C
REG_CAN1_IER         = 0x40006414
REG_CAN2_MSR         = 0x40006804
REG_CAN2_BTR         = 0x4000681C
REG_CAN2_IER         = 0x40006814
REG_CAN_FA1R         = 0x4000661C  # Filter Active Reg

def inject_can_frame(can_id, data_bytes):
    """
    Injects a CAN frame directly into the MCU's CAN RX ring buffer in SRAM,
    advancing the head pointer so the cooperative dispatcher (Task 1) will consume it.
    Also asserts the CAN activity flag to wake the power manager matching physical ISR behavior.
    """
    slot_addr = SYM_CAN_RING
    payload = bytearray(20)
    struct.pack_into("<I", payload, 0, can_id)
    struct.pack_into("<I", payload, 4, 0)
    payload[8] = 0   # standard ID
    payload[9] = 0   # data frame
    payload[10] = len(data_bytes)
    for i, b in enumerate(data_bytes[:8]):
        payload[11 + i] = b
    payload[19] = 0

    cmds = [
        "init",
        "halt",
        f"mwb {SYM_CAN_TAIL:#x} 0",
        f"mwb {SYM_CAN_HEAD:#x} 0",
    ]
    for idx, byte in enumerate(payload):
        cmds.append(f"mwb {slot_addr + idx:#x} {byte:#x}")
    cmds.append(f"mwb {SYM_CAN_HEAD:#x} 1")
    cmds.append(f"mwb {SYM_CAN_ACTIVITY:#x} 1")
    cmds.append(f"mwb {SYM_POWER_STATE:#x} 1")  # byte write -- see SYM_POWER_STATE's own comment
    cmds.append(f"mwh {SYM_TIMER_COUNTER:#x} 0")
    cmds.append("resume")
    run_ocd_commands(cmds)

def main():
    print("=" * 70)
    print(" STM32F105 Companion MCU — Comprehensive HIL Hardware Test Suite")
    print("=" * 70)
    
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

    # Boot the MCU cleanly into application
    run_ocd_commands(["init", "reset run"])
    time.sleep(0.15)

    # TEST 1: Core Health, Exception Flags, and Fault Registers
    print("\n>>> 1. Core Health & System Fault Registers")
    words = read_mem_words(REG_CFSR, 2)
    cfsr = words[0] if words else 0xFFFFFFFF
    hfsr = words[1] if len(words) > 1 else 0xFFFFFFFF
    assert_test(cfsr == 0, "CFSR Fault Register is Zero", f"CFSR = 0x{cfsr:08X}")
    assert_test(hfsr == 0, "HFSR HardFault Register is Zero", f"HFSR = 0x{hfsr:08X}")

    # TEST 2: Clock Tree & PLL Stability
    print("\n>>> 2. Clock Tree & PLL Lock")
    rcc = read_mem_words(REG_RCC_CR, 2)
    cr = rcc[0] if len(rcc) > 0 else 0
    cfgr = rcc[1] if len(rcc) > 1 else 0
    hse_rdy = (cr & (1 << 17)) != 0
    pll_rdy = (cr & (1 << 25)) != 0
    sws_pll = ((cfgr >> 2) & 0x03) == 0x02  # SWS=10 -> PLL used as SYSCLK
    assert_test(hse_rdy and pll_rdy, "HSE 25MHz & PLL Clock Locked", f"CR = 0x{cr:08X}")
    assert_test(sws_pll, "SYSCLK Driven by PLL (72 MHz)", f"CFGR = 0x{cfgr:08X}")

    # TEST 3: SysTick 1.000 kHz Hardware Timebase
    print("\n>>> 3. Millisecond SysTick Engine Progression")
    t_start = time.time()
    t1_list = read_mem_words(SYM_SYSTEM_TICKS, 1)
    t1 = t1_list[0] if t1_list else 0
    time.sleep(1.0)
    t2_list = read_mem_words(SYM_SYSTEM_TICKS, 1)
    t2 = t2_list[0] if t2_list else 0
    elapsed_ms = (time.time() - t_start) * 1000.0
    delta = t2 - t1
    assert_test(abs(delta - elapsed_ms) < 250, f"SysTick Progressed Linearly (~1000 Hz): delta = {delta} ms (elapsed = {elapsed_ms:.0f} ms)", f"t1={t1}, t2={t2}")

    # TEST 4: Dual bxCAN Peripheral States (CAN1 & CAN2)
    print("\n>>> 4. Dual bxCAN Peripheral Registers (CAN1 & CAN2)")
    can1_msr = read_mem_words(REG_CAN1_MSR, 1)[0]
    can1_btr = read_mem_words(REG_CAN1_BTR, 1)[0]
    can1_ier = read_mem_words(REG_CAN1_IER, 1)[0]
    
    can2_msr = read_mem_words(REG_CAN2_MSR, 1)[0]
    can2_btr = read_mem_words(REG_CAN2_BTR, 1)[0]
    can2_ier = read_mem_words(REG_CAN2_IER, 1)[0]
    
    fa1r = read_mem_words(REG_CAN_FA1R, 1)[0]
    
    assert_test((can1_msr & 0x03) == 0, "CAN1 in Normal Operating Mode (Not Sleep/Init)", f"MSR=0x{can1_msr:08X}")
    assert_test((can2_msr & 0x03) == 0, "CAN2 in Normal Operating Mode (Not Sleep/Init)", f"MSR=0x{can2_msr:08X}")
    assert_test(can1_btr == 0x00140008, "CAN1 500 kbps @ 75% Sample Point Timing", f"BTR=0x{can1_btr:08X}")
    assert_test(can2_btr == 0x00140008, "CAN2 500 kbps @ 75% Sample Point Timing", f"BTR=0x{can2_btr:08X}")
    assert_test((can1_ier & 0x02) != 0 and (can2_ier & 0x02) != 0, "CAN1 & CAN2 FMPIE0 Interrupts Active", f"IER1=0x{can1_ier:08X}, IER2=0x{can2_ier:08X}")
    assert_test((fa1r & (1 << 0)) != 0 and (fa1r & (1 << 14)) != 0, "CAN Filter Bank 0 (CAN1) & Bank 14 (CAN2) Active", f"FA1R=0x{fa1r:08X}")

    # TEST 5: AFIO Remap & Power Manager Active State
    print("\n>>> 5. AFIO Pin Remap & Power Manager Executive")
    mapr = read_mem_words(REG_AFIO_MAPR, 1)[0]
    swj_cfg = (mapr >> 24) & 0x07
    assert_test(swj_cfg == 0x02, "SWJ_CFG Remapped (JTAG Disabled, SW-DP Enabled for PB3/PB4)", f"MAPR=0x{mapr:08X}")
    
    pwr_state = read_mem_bytes(SYM_POWER_STATE, 1)[0]
    assert_test(pwr_state in (1, 2), f"MCU Power Management State is ACTIVE or STANDBY_WAIT (State {pwr_state})")

    # TEST 6: CAN Steering Wheel Angle (0x025) Decoding
    print("\n>>> 6. CAN Steering Wheel Angle (0x025) Decoding")
    # Left turn: data[0] bit 3 set (0x08). raw = (0x0FFF - 0x0800) = 0x07FF. scaled = 0x03FF -> clamped to 0x7F, sign = 0.
    inject_can_frame(0x025, [0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    assert_test(sign == 0 and mag == 0x7F, f"Left Turn Clamped: sign={sign}, mag=0x{mag:02X} (Expected 0, 0x7F)")

    # Right turn: data[0] bit 3 clear (0x00). raw = 0x0040. scaled = 0x20, sign = 1.
    inject_can_frame(0x025, [0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    assert_test(sign == 1 and mag == 0x20, f"Right Turn: sign={sign}, mag=0x{mag:02X} (Expected 1, 0x20)")

    # TEST 7: CAN Transmission & Reverse Gear (0x1D0) Decoding
    print("\n>>> 7. CAN Transmission & Reverse Gear (0x1D0) Decoding")
    inject_can_frame(0x1D0, [0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    rev = read_mem_bytes(SYM_LAST_REVERSE, 1)[0]
    gear = read_mem_bytes(SYM_GEAR_FIELD, 1)[0]
    stat = read_mem_bytes(SYM_VEHICLE_STATUS, 1)[0]
    assert_test(rev == 1 and gear == 0x20 and (stat & 0x04) != 0,
                f"Reverse Engaged: rev={rev}, gear=0x{gear:02X}, stat=0x{stat:02X} (Expected bit 2 set)")

    inject_can_frame(0x1D0, [0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    rev = read_mem_bytes(SYM_LAST_REVERSE, 1)[0]
    gear = read_mem_bytes(SYM_GEAR_FIELD, 1)[0]
    stat = read_mem_bytes(SYM_VEHICLE_STATUS, 1)[0]
    assert_test(rev == 0 and gear == 0x10 and (stat & 0x04) == 0,
                f"Reverse Disengaged: rev={rev}, gear=0x{gear:02X}, stat=0x{stat:02X} (Expected bit 2 clear)")

    # TEST 8: CAN Parking Radar (0x396) Distance Mapping & Middle Duplication
    print("\n>>> 8. CAN Parking Radar (0x396) 6-Sensor Matrix & 8-Channel Map")
    inject_can_frame(0x396, [0x00, 0x14, 0x23, 0x51, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    front = read_mem_bytes(SYM_FRONT_RADAR, 4)
    rear = read_mem_bytes(SYM_REAR_RADAR, 4)
    expected_front = [1, 5, 5, 8]  # FL=1, FML=5, FMR=5, FR=8
    expected_rear = [8, 5, 5, 1]   # RL=8, RML=5, RMR=5, RR=1
    assert_test(front == expected_front, f"Front Radar Mapped: {front} (Expected {expected_front})")
    assert_test(rear == expected_rear, f"Rear Radar Mapped: {rear} (Expected {expected_rear})")

    # TEST 9: CAN Body Controller (0x622) Headlights & Doors
    print("\n>>> 9. CAN Body Controller (0x622) Headlights & Door Debounce")
    inject_can_frame(0x622, [0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00])
    time.sleep(0.15)
    lights = read_mem_bytes(SYM_LAST_LIGHTS, 1)[0]
    stat = read_mem_bytes(SYM_VEHICLE_STATUS, 1)[0]
    assert_test(lights == 1 and (stat & 0x02) != 0, f"Headlights ON: lights={lights}, stat=0x{stat:02X} (Expected bit 1 set)")

    for _ in range(3):
        inject_can_frame(0x622, [0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00])
        time.sleep(0.05)
    time.sleep(0.1)
    door = read_mem_bytes(SYM_DOOR_OPEN, 1)[0]
    last_door = read_mem_bytes(SYM_LAST_DOOR, 1)[0]
    assert_test(door == 1 and last_door == 1, f"Door Ajar Debounced & Latched: door={door}, last={last_door}")

    # TEST 10: Sustained Execution & Watchdog Stability (5.0s run)
    print("\n>>> 10. Sustained Execution & Watchdog (IWDG) Stability Run")
    t_ticks_start = read_mem_words(SYM_SYSTEM_TICKS, 1)[0]
    print(" Running continuously for 5.0 seconds on physical silicon...")
    time.sleep(5.0)
    words_end = read_mem_words(REG_CFSR, 2)
    cfsr_end = words_end[0] if words_end else 0xFFFFFFFF
    ticks_end = read_mem_words(SYM_SYSTEM_TICKS, 1)[0]
    delta_sustained = ticks_end - t_ticks_start
    assert_test(cfsr_end == 0, f"No Faults After Sustained Execution (CFSR=0x{cfsr_end:08X})")
    assert_test(delta_sustained >= 4500, f"SysTick Active Across Test Run: delta = {delta_sustained} ms (ticks={ticks_end})")

    print("\n" + "=" * 70)
    print(f" HIL Test Summary: {passed}/{total} Tests Passed ({100.0 * passed / total:.1f}%)")
    print("=" * 70)
    
    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
