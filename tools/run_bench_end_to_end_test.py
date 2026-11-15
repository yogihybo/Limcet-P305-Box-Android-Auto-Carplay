#!/usr/bin/env python3
"""
End-to-End Physical Silicon Bench Test Routine
Matches docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md Section 40:
  1. Flash combined_factory_boot_factory_app_64k.bin over SWD -> verify boot & CMD 0x81
  2. In-place UART upgrade over physical wire to clean-room app (build_bench/can_app.bin)
     -> SWD bit-for-bit readback & verify clean-room CMD 0x81 telemetry
  3. In-place UART restore over physical wire to factory app (live_factory_app_52k_reconstructed.bin)
  4. SWD bit-for-bit readback of 64KB & 128KB -> verify restored factory app & CMD 0x81
  5. Release ignition -> verify parked state (2 MHz HSI, USART2 off, factory GPIO ODR levels)
"""

import sys
import os
import time
import subprocess
import threading
import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "tools", "pico_stm32.cfg")
PORT = "/dev/ttyACM0"
BAUD = 38400

FACTORY_COMBINED_BIN = os.path.join(ROOT, "hardware", "MCU", "live_dumps", "combined_factory_boot_factory_app_64k.bin")
FACTORY_APP_BIN = os.path.join(ROOT, "hardware", "MCU", "live_dumps", "vehicle_live_2026-09-14", "live_factory_app_52k_reconstructed.bin")
CLEANROOM_BENCH_BIN = os.path.join(ROOT, "hardware", "MCU", "source", "build_bench", "can_app.bin")


def run_ocd(*cmds):
    args = ["openocd", "-f", CFG]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "exit"]
    res = subprocess.run(args, capture_output=True, text=True, timeout=60)
    out = res.stdout + "\n" + res.stderr
    if res.returncode != 0:
        raise RuntimeError(f"OpenOCD failed (code {res.returncode}):\n{out}")
    return out


def mrw(addr):
    out = run_ocd("init", f"echo VAL:[mrw 0x{addr:08X}]")
    for line in out.splitlines():
        if "VAL:" in line:
            return int(line.split("VAL:")[1].strip(), 0)
    raise ValueError(f"Could not read word at 0x{addr:08X}: {out}")


def get_core_status():
    out = run_ocd(
        "init",
        "echo VTOR:[mrw 0xE000ED08]",
        "echo CFSR:[mrw 0xE000ED28]",
        "echo HFSR:[mrw 0xE000ED2C]"
    )
    vtor, cfsr, hfsr = 0, 0, 0
    for line in out.splitlines():
        if "VTOR:" in line:
            vtor = int(line.split("VTOR:")[1].strip(), 0)
        elif "CFSR:" in line:
            cfsr = int(line.split("CFSR:")[1].strip(), 0)
        elif "HFSR:" in line:
            hfsr = int(line.split("HFSR:")[1].strip(), 0)
    return vtor, cfsr, hfsr


def get_pc():
    out = run_ocd("init", "mww 0xE0042004 0x00000307", "halt", "reg pc", "resume")
    for line in out.splitlines():
        if line.strip().lower().startswith("pc (") or line.strip().lower().startswith("pc:"):
            return int(line.split(":")[1].strip(), 16)
    return 0


def wake_factory_app():
    """Switches PD2 to pull-down (GPIOD->BRR = 4) without halting the core."""
    run_ocd("init", "mww 0x40011414 4", "mww 0xE0042004 0x00000307")
    time.sleep(0.2)


def wait_for_vtor(expected_vtor=0x08003000, timeout_s=6.0):
    """Waits for MCU to reset out of bootloader and latch expected VTOR."""
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        try:
            vtor, _, _ = get_core_status()
            if vtor == expected_vtor:
                return True
        except Exception:
            pass
        time.sleep(0.4)
    return False


def wait_for_factory_state_3(timeout_s=10.0):
    """Waits for factory app settle sequence (State 1: 50ms, State 2: 500ms) to reach State 3."""
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        wake_factory_app()
        st = mrw(0x200007E8) & 0xFF
        if st == 3:
            return True
        time.sleep(0.3)
    return False


def wait_for_parked_state_5(timeout_s=15.0):
    """Waits for 5.0s ACC timeout + power-down to enter State 5 (Parked) and completes 100ms settle."""
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        st = mrw(0x200007E8) & 0xFF
        if st == 5:
            # Allow 1.5s for the 100ms State 5 delay, clock-down, and pin configuration to complete
            time.sleep(1.5)
            return True
        time.sleep(0.5)
    return False


def release_ignition():
    """Switches PD2 to pull-up (GPIOD->BSRR = 4) allowing natural 5.0s ACC timeout with zero timer injection."""
    run_ocd("init", "mww 0x40011410 4")


def query_cmd_81(timeout_s=1.0, retries=5):
    """Sends CMD 0x81 (2E 81 01 01 7C) with retries and collects parsed 0x2E frames."""
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    pkt = bytes([0x2E, 0x81, 0x01, 0x01, 0x7C])

    for attempt in range(retries):
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        ser.write(pkt)
        ser.flush()

        deadline = time.time() + timeout_s
        buf = b""
        frames = []

        while time.time() < deadline:
            chunk = ser.read(64)
            if chunk:
                buf += chunk
                while True:
                    idx = buf.find(b"\x2E")
                    if idx < 0:
                        buf = b""
                        break
                    if len(buf) < idx + 3:
                        break
                    cmd = buf[idx + 1]
                    length = buf[idx + 2]
                    total = idx + 3 + length + 1
                    if len(buf) < total:
                        break
                    payload = buf[idx + 3 : idx + 3 + length]
                    chk = buf[idx + 3 + length]
                    expected_chk = (~(cmd + length + sum(payload))) & 0xFF
                    if chk == expected_chk:
                        frames.append((cmd, payload))
                        buf = buf[total:]
                    else:
                        buf = buf[idx + 1 :]
            else:
                time.sleep(0.02)

        if frames:
            ser.close()
            return frames
        time.sleep(0.3)

    ser.close()
    return frames


def main():
    print("=" * 76)
    print(" STM32F105 PHYSICAL SILICON END-TO-END BENCH TEST ROUTINE")
    print(" Reference: docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md Section 40")
    print("=" * 76)

    # -------------------------------------------------------------------------
    # STEP 1: Flash combined_factory_boot_factory_app_64k.bin over SWD
    # -------------------------------------------------------------------------
    print("\n[STEP 1] Flash combined factory bootloader + factory app over SWD...")
    t0 = time.time()
    run_ocd(
        "init",
        "reset init",
        f"flash write_image erase {FACTORY_COMBINED_BIN} 0x08000000",
        f"verify_image {FACTORY_COMBINED_BIN} 0x08000000",
        "reset init",
        "resume"
    )
    print(f" [+] SWD programming & verification complete ({time.time() - t0:.2f}s)")

    # Allow bootloader and factory app startup/gpio_init to settle before setting ignition
    time.sleep(1.0)
    # Wake factory app by pulling PD2 low (ignition active)
    wake_factory_app()
    print(" [*] Waiting for factory app settle steps (State 1 & 2 -> State 3 Running)...")
    assert wait_for_factory_state_3(10.0), "Factory app failed to enter State 3 (Running)!"
    print(" [+] Factory app reached State 3 (Running).")

    # Verify core status without halting
    vtor, cfsr, hfsr = get_core_status()
    print(f" [+] Core state: VTOR=0x{vtor:08X}, CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}")
    assert vtor == 0x08003000, f"Expected VTOR 0x08003000, got 0x{vtor:08X}"
    assert cfsr == 0 and hfsr == 0, f"Fault detected: CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}"

    # Query CMD 0x81 over serial
    print(" [*] Querying factory app with CMD 0x81 over UART (/dev/ttyACM0 @ 38400)...")
    step1_frames = query_cmd_81()
    print(f" [+] Received {len(step1_frames)} frame(s) from factory app:")
    version_found = False
    for cmd, p in step1_frames:
        print(f"     CMD 0x{cmd:02X} (len {len(p)}): {p.hex()}", end="")
        if cmd == 0x7F:
            ver_str = p.decode("ascii", errors="replace")
            print(f" -> '{ver_str}'", end="")
            if "Limcet-V1.0-1302" in ver_str:
                version_found = True
        print()
    assert version_found, "Factory app did not return expected Limcet-V1.0-1302 version string!"
    print(" [+] Step 1 PASSED: Factory stack running, verified via SWD and UART.")

    # -------------------------------------------------------------------------
    # STEP 2: In-place UART update to clean-room app (build_bench/can_app.bin)
    # -------------------------------------------------------------------------
    print("\n[STEP 2] In-place UART update to clean-room app (build_bench/can_app.bin)...")

    sender_cmd = [
        sys.executable,
        os.path.join(ROOT, "tools", "mcu_update_sender.py"),
        "-p", PORT,
        "-b", str(BAUD),
        CLEANROOM_BENCH_BIN
    ]
    t0 = time.time()
    res = subprocess.run(sender_cmd, capture_output=True, text=True, timeout=60)
    print(res.stdout)
    if res.returncode != 0:
        print(res.stderr, file=sys.stderr)
        raise RuntimeError("mcu_update_sender failed!")
    print(f" [+] UART flashing complete in {time.time() - t0:.2f}s")

    # Give board time to watchdog reset into clean-room app (~2.0s)
    print(" [*] Waiting for bootloader watchdog reset into new application (~2.0s)...")
    assert wait_for_vtor(0x08003000, timeout_s=6.0), "MCU failed to boot into clean-room app (VTOR != 0x08003000)!"

    # Read back flash over SWD to verify byte-for-byte exactness
    app_len = os.path.getsize(CLEANROOM_BENCH_BIN)
    dump_tmp = "/tmp/cleanroom_readback.bin"
    run_ocd("init", "mww 0xE0042004 0x00000307", "halt", f"dump_image {dump_tmp} 0x08003000 {app_len}", "resume")
    with open(CLEANROOM_BENCH_BIN, "rb") as f:
        expected_bytes = f.read()
    with open(dump_tmp, "rb") as f:
        actual_bytes = f.read()
    assert actual_bytes == expected_bytes, "Flash readback does NOT match build_bench/can_app.bin byte-for-byte!"
    print(f" [+] SWD Flash readback: {app_len} bytes at 0x08003000 match build_bench/can_app.bin BIT-FOR-BIT!")

    # Check clean-room core state
    vtor, cfsr, hfsr = get_core_status()
    print(f" [+] Clean-room app running: VTOR=0x{vtor:08X}, CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}")
    assert vtor == 0x08003000
    assert cfsr == 0 and hfsr == 0

    # Query clean-room app via CMD 0x81
    print(" [*] Querying clean-room app with CMD 0x81 over UART...")
    step2_frames = query_cmd_81()
    print(f" [+] Received {len(step2_frames)} frame(s) from clean-room app:")
    cr_ver_found = False
    for cmd, p in step2_frames:
        print(f"     CMD 0x{cmd:02X} (len {len(p)}): {p.hex()}", end="")
        if cmd == 0x7F:
            ver_str = p.decode("ascii", errors="replace")
            print(f" -> '{ver_str}'", end="")
            if "Limcet-V1.0-1302" in ver_str:
                cr_ver_found = True
        print()
    assert cr_ver_found, "Clean-room app did not return expected Limcet-V1.0-1302 version string!"
    print(" [+] Step 2 PASSED: Clean-room app programmed in-place over UART and verified running.")

    # -------------------------------------------------------------------------
    # STEP 3: In-place UART restore to factory app (live_factory_app_52k_reconstructed.bin)
    # -------------------------------------------------------------------------
    print("\n[STEP 3] In-place UART restore of factory application (52KB)...")
    sender_cmd = [
        sys.executable,
        os.path.join(ROOT, "tools", "mcu_update_sender.py"),
        "-p", PORT,
        "-b", str(BAUD),
        FACTORY_APP_BIN
    ]
    t0 = time.time()
    res = subprocess.run(sender_cmd, capture_output=True, text=True, timeout=90)
    print(res.stdout)
    if res.returncode != 0:
        print(res.stderr, file=sys.stderr)
        raise RuntimeError("Restore mcu_update_sender failed!")
    print(f" [+] Factory app restore complete in {time.time() - t0:.2f}s")

    # -------------------------------------------------------------------------
    # STEP 4: SWD Readback & Verification of Restored Factory Stack
    # -------------------------------------------------------------------------
    print("\n[STEP 4] SWD Readback verification of restored factory stack...")
    # Allow restore watchdog reset into factory app
    print(" [*] Waiting for bootloader watchdog reset into restored application (~2.0s)...")
    assert wait_for_vtor(0x08003000, timeout_s=6.0), "MCU failed to boot into restored factory app (VTOR != 0x08003000)!"
    # Wake restored factory app
    wake_factory_app()
    print(" [*] Waiting for restored factory app settle steps (State 1 & 2 -> State 3 Running)...")
    assert wait_for_factory_state_3(10.0), "Restored factory app failed to enter State 3 (Running)!"
    print(" [+] Restored factory app reached State 3 (Running).")

    dump_64k = "/tmp/restored_factory_64k.bin"
    run_ocd("init", "mww 0xE0042004 0x00000307", "halt", f"dump_image {dump_64k} 0x08000000 65536", "resume")
    with open(FACTORY_COMBINED_BIN, "rb") as f:
        expected_64k = f.read()
    with open(dump_64k, "rb") as f:
        actual_64k = f.read()
    assert actual_64k == expected_64k, "Restored 64KB flash does NOT match combined_factory_boot_factory_app_64k.bin!"
    print(" [+] First 64KB of Flash matches combined_factory_boot_factory_app_64k.bin BIT-FOR-BIT!")

    # Verify upper 64KB (0x08010000 - 0x0801FFFF) is clean erased (0xFF)
    dump_upper = "/tmp/upper_flash_64k.bin"
    run_ocd("init", "mww 0xE0042004 0x00000307", "halt", f"dump_image {dump_upper} 0x08010000 65536", "resume")
    with open(dump_upper, "rb") as f:
        upper_bytes = f.read()
    assert all(b == 0xFF for b in upper_bytes), "Upper flash (0x08010000+) was not fully erased!"
    print(" [+] Upper 64KB (0x08010000-0x0801FFFF) verified clean erased (all 0xFF).")

    # Check restored factory app core state
    vtor, cfsr, hfsr = get_core_status()
    print(f" [+] Restored factory app: VTOR=0x{vtor:08X}, CFSR=0x{cfsr:08X}, HFSR=0x{hfsr:08X}")
    assert vtor == 0x08003000
    assert cfsr == 0 and hfsr == 0

    # Query CMD 0x81 and compare with Step 1
    print(" [*] Querying restored factory app with CMD 0x81 over UART...")
    step4_frames = query_cmd_81()
    print(f" [+] Received {len(step4_frames)} frame(s) from restored factory app:")
    restored_ver_found = False
    for cmd, p in step4_frames:
        print(f"     CMD 0x{cmd:02X} (len {len(p)}): {p.hex()}", end="")
        if cmd == 0x7F:
            ver_str = p.decode("ascii", errors="replace")
            print(f" -> '{ver_str}'", end="")
            if "Limcet-V1.0-1302" in ver_str:
                restored_ver_found = True
        print()
    assert restored_ver_found, "Restored factory app did not return expected Limcet-V1.0-1302 version string!"
    print(" [+] Restored factory app UART responses verified!")
    print(" [+] Step 4 PASSED: Factory application restored and verified 100%.")

    # -------------------------------------------------------------------------
    # STEP 5: Release Ignition & Verify Parked State
    # -------------------------------------------------------------------------
    print("\n[STEP 5] Release ignition and verify factory low-power parked state...")
    release_ignition()
    print(" [*] Ignition released. Waiting for 5.0s ACC timeout + park (~8s @ 8MHz HSI)...")
    assert wait_for_parked_state_5(15.0), "MCU failed to enter State 5 (Parked)!"
    print(" [+] MCU entered State 5 (Parked).")

    # Read core and peripheral state in parked mode
    pc = get_pc()
    cfgr = mrw(0x40021004)
    u2_cr1 = mrw(0x4000440C)
    gpiob_odr = mrw(0x40010C0C)
    gpioc_odr = mrw(0x4001100C)
    state_byte = mrw(0x200007E8) & 0xFF

    print(f" [+] Parked state check:")
    print(f"     PC:         0x{pc:08X} (Parked loop 0x08007CCA - 0x08007CE6)")
    print(f"     State byte: {state_byte} (Expected state 5)")
    print(f"     RCC->CFGR:  0x{cfgr:08X} (Expected 0x...4090: HSI, AHB /4, 2 MHz)")
    print(f"     USART2->CR1:0x{u2_cr1:08X} (UE bit 13 = 0, disabled)")
    print(f"     GPIOB->ODR: 0x{gpiob_odr:08X} (Expected 0xD006: PB2, PB14, PB15 high; PB3, PB4, PB5 low)")
    print(f"     GPIOC->ODR: 0x{gpioc_odr:08X} (Expected 0x0000: all low)")

    # Assert parked invariants matching Section 40
    assert (0x08007CCA <= pc <= 0x08007CE6) or (0x08005006 <= pc <= 0x08005016) or (0x08006EAC <= pc <= 0x08006EB6), f"PC 0x{pc:08X} not in parked loop!"
    assert state_byte == 5, f"Expected state 5 at 0x200007E8, got {state_byte}"
    assert (cfgr & 0xF0) == 0x90, f"Expected AHB prescaler /4 (0x90), got 0x{cfgr:08X}"
    assert (cfgr & 0x0C) == 0x00, f"Expected SYSCLK on HSI, got 0x{cfgr:08X}"
    assert (u2_cr1 & (1 << 13)) == 0, "USART2 was not disabled in parked mode!"
    assert (gpiob_odr & (1 << 2)) != 0, "PB2 should be HIGH"
    assert (gpiob_odr & ((1 << 3) | (1 << 4) | (1 << 5))) == 0, "PB3, PB4, PB5 should be LOW"
    assert (gpiob_odr & ((1 << 14) | (1 << 15))) == ((1 << 14) | (1 << 15)), "PB14, PB15 should be HIGH"
    assert (gpioc_odr & 0xFFFF) == 0x0000, f"GPIOC ODR expected 0x0000, got 0x{gpioc_odr:04X}"

    print(" [+] Step 5 PASSED: Parked levels, 2 MHz clock-down, and loop verified.")

    print("\n" + "=" * 76)
    print(" ALL 5 PHASES OF BENCH TEST ROUTINE PASSED 100% WITH ZERO FAULTS!")
    print("=" * 76)
    return 0


if __name__ == "__main__":
    sys.exit(main())
