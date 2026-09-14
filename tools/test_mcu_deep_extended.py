#!/usr/bin/env python3
"""
Extended deep HIL test suite for the STM32F105 companion MCU (clean-room
can_app.bin, Mode 1 / Toyota Prado 150 profile).

Builds on tools/test_mcu_rigorous.py's proven CAN-ring-buffer injection
mechanism (23/23 passing baseline) to cover territory that suite doesn't:
boundary/edge-case input values, rapid back-to-back multi-frame sequencing
(dispatcher/ring-buffer robustness under load), and debounce logic that
must NOT trigger on a single transient reading.

Does not modify or duplicate test_mcu_rigorous.py -- imports its helpers.

=== KNOWN, UNRESOLVED ISSUE (2026-09-14) -- READ BEFORE TRUSTING RESULTS ===
This suite is currently UNRELIABLE and its FAIL output should NOT be taken
as confirmed firmware bugs. Investigation this session found:

  - Immediately after a fresh board reset, single CAN-frame injection via
    inject_can_frame() works perfectly and reliably (verified repeatedly).
  - After the board has been running for some period (observed: reliably
    broken well before this suite's later tests run; a fresh single-shot
    injection at ~10s uptime showed a PARTIAL, inconsistent update rather
    than a clean pass or a clean stuck value), further CAN injections stop
    being consumed by the firmware -- SRAM writes to the ring buffer verified
    byte-correct via direct readback, but the decoded state never updates.
  - Ruled out: general system fault (CFSR/HFSR stayed 0 throughout), CAN1
    hardware bus noise/errors (RF0R/RF1R/ESR all read 0, no real frames or
    errors accumulating), and a global scheduler stall (SysTick continued
    ticking at ~1kHz and single-stepping showed the CPU genuinely executing
    a live idle/dispatch loop, not stuck).
  - NOT ruled out / not yet understood: whether this is a genuine firmware
    behavior under sustained uptime (a real bug worth knowing about for
    always-on vehicle operation) or an artifact of this test methodology
    itself -- e.g. many repeated external SWD halt/resume cycles across a
    session perturbing timing-sensitive scheduler/power-manager state in a
    way a real, undisturbed vehicle boot never would.
  - tools/test_mcu_rigorous.py itself remains reliably 100% passing when
    run as a single fresh process end-to-end; this doesn't call that
    result into question. The issue is specific to ad-hoc/standalone
    injection calls made well after boot, in a separate process, following
    a long prior session of SWD activity.

Next step for whoever picks this up: reproduce with a clean, uninterrupted
board (no prior SWD sessions this power-cycle) and binary-search the exact
uptime/interaction threshold where injection stops being consumed, ideally
correlating against a genuine internal firmware symbol (not just wall-clock
guesses) to distinguish "real bug" from "test methodology artifact".
"""

import sys
import time

sys.path.insert(0, "tools")
from test_mcu_rigorous import (
    run_ocd_commands, read_mem_words, read_mem_bytes, inject_can_frame,
    SYM_REAR_RADAR, SYM_FRONT_RADAR, SYM_VEHICLE_STATUS,
    SYM_DOOR_OPEN, SYM_GEAR_FIELD, SYM_STEERING_MAG, SYM_STEERING_SIGN,
    SYM_LAST_REVERSE, SYM_LAST_LIGHTS, SYM_LAST_DOOR,
    REG_CFSR, REG_HFSR,
)

passed = 0
total = 0

def check(cond, desc, detail=""):
    global passed, total
    total += 1
    if cond:
        passed += 1
        print(f" [PASS] Test {total:02d}: {desc}")
    else:
        print(f" [FAIL] Test {total:02d}: {desc} -- {detail}")

def main():
    print("=" * 70)
    print(" STM32F105 Companion MCU -- Extended Deep I/O Simulation Suite")
    print("=" * 70)

    # ---- 1. Steering angle: exact clamp boundary from both directions ----
    print("\n>>> 1. Steering Angle Boundary Values (CAN 0x025)")
    # raw 12-bit magnitude 0xFF -> scaled = (0xFF>>1) = 0x7F, exactly at the clamp
    inject_can_frame(0x025, [0x00, 0xFF, 0, 0, 0, 0, 0, 0])
    time.sleep(0.15)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    check(sign == 1 and mag == 0x7F, f"Right turn at exact clamp boundary: sign={sign}, mag=0x{mag:02X}", "expected sign=1 mag=0x7F")

    # raw magnitude that would exceed 0x7F pre-clamp (0x0FFF near max) -> must clamp to 0x7F
    inject_can_frame(0x025, [0x0F, 0xFF, 0, 0, 0, 0, 0, 0])
    time.sleep(0.15)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    check(mag == 0x7F, f"Over-range magnitude correctly clamped: sign={sign}, mag=0x{mag:02X}", "expected mag=0x7F (clamped, no wraparound/overflow)")

    # zero magnitude (dead-center steering) -> sign bit clear, mag 0
    inject_can_frame(0x025, [0x00, 0x00, 0, 0, 0, 0, 0, 0])
    time.sleep(0.15)
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    check(mag == 0x00, f"Dead-center steering: sign={sign}, mag=0x{mag:02X}", "expected mag=0x00")

    # ---- 2. Radar boundary values: 0 (no obstacle), 1 (closest), 7 (max valid), 8-15 (out of table range) ----
    print("\n>>> 2. Parking Radar Boundary Values (CAN 0x396)")
    # all sensors = 0 -> should map to 12 (0x0C, "no obstacle / off") per the confirmed lookup table
    inject_can_frame(0x396, [0, 0x00, 0x00, 0x00, 0, 0, 0, 0])
    time.sleep(0.15)
    front = read_mem_bytes(SYM_FRONT_RADAR, 4)
    rear = read_mem_bytes(SYM_REAR_RADAR, 4)
    check(front == [12, 12, 12, 12] and rear == [12, 12, 12, 12],
          f"All-zero radar -> no-obstacle mapping: front={front}, rear={rear}",
          "expected all channels = 12 (0x0C)")

    # all sensors = 1 (closest/red) on all 4 nibbles -> all channels should map to 1
    inject_can_frame(0x396, [0, 0x11, 0x11, 0x11, 0, 0, 0, 0])
    time.sleep(0.15)
    front = read_mem_bytes(SYM_FRONT_RADAR, 4)
    rear = read_mem_bytes(SYM_REAR_RADAR, 4)
    check(front == [1, 1, 1, 1] and rear == [1, 1, 1, 1],
          f"All-closest radar -> level 1 mapping: front={front}, rear={rear}",
          "expected all channels = 1")

    # out-of-table nibble value (e.g. 0xF, beyond the documented 0-7 real range) -> must not crash, should fall to default (12)
    inject_can_frame(0x396, [0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0])
    time.sleep(0.15)
    cfsr = read_mem_words(REG_CFSR, 1)[0]
    front = read_mem_bytes(SYM_FRONT_RADAR, 4)
    check(cfsr == 0, f"Out-of-range radar nibble (0xF) doesn't fault: CFSR=0x{cfsr:08X}, front={front}", "expected CFSR=0 (robust default-case handling)")

    # ---- 3. Door debounce: a single transient pulse must NOT latch ----
    print("\n>>> 3. Door Ajar Debounce -- Transient Rejection (CAN 0x622)")
    # first clear door state with a clean "closed" frame, confirm baseline
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x00, 0, 0])
    time.sleep(0.15)
    door0 = read_mem_bytes(SYM_LAST_DOOR, 1)[0]
    # single transient "open" reading, immediately followed by "closed" again (debounce needs 2 consecutive)
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x10, 0, 0])
    time.sleep(0.15)
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x00, 0, 0])
    time.sleep(0.15)
    door_after_transient = read_mem_bytes(SYM_LAST_DOOR, 1)[0]
    check(door_after_transient == door0, f"Single transient door pulse did not latch: before={door0}, after={door_after_transient}", "debounce should have rejected a 1-sample blip")

    # now genuinely hold door open across 2+ consecutive frames -> must latch
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x10, 0, 0])
    time.sleep(0.15)
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x10, 0, 0])
    time.sleep(0.15)
    door_latched = read_mem_bytes(SYM_LAST_DOOR, 1)[0]
    check(door_latched == 1, f"Sustained door-open correctly latches: last_door={door_latched}", "expected 1 after 2 consecutive open readings")
    # clean up: close the door again for subsequent tests
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x00, 0, 0])
    time.sleep(0.15)
    inject_can_frame(0x622, [0, 0, 0, 0x00, 0, 0x00, 0, 0])
    time.sleep(0.15)

    # ---- 4. Rapid back-to-back multi-frame sequencing (dispatcher/ring-buffer stress) ----
    print("\n>>> 4. Rapid Multi-Frame Sequencing (dispatcher/ring-buffer under load)")
    # fire 4 different real CAN IDs back-to-back with minimal delay, then verify
    # the FINAL state reflects each one correctly (nothing dropped/corrupted)
    inject_can_frame(0x025, [0x00, 0x40, 0, 0, 0, 0, 0, 0])   # steering: right, mid-range
    inject_can_frame(0x1D0, [0, 0, 0, 0x20, 0, 0, 0, 0])       # reverse engaged
    inject_can_frame(0x396, [0, 0x22, 0x22, 0x22, 0, 0, 0, 0]) # radar level 2 all channels
    inject_can_frame(0x622, [0, 0, 0, 0x10, 0, 0x00, 0, 0])    # lights on
    time.sleep(0.15)
    cfsr = read_mem_words(REG_CFSR, 1)[0]
    sign = read_mem_bytes(SYM_STEERING_SIGN, 1)[0]
    mag = read_mem_bytes(SYM_STEERING_MAG, 1)[0]
    rev = read_mem_bytes(SYM_LAST_REVERSE, 1)[0]
    front = read_mem_bytes(SYM_FRONT_RADAR, 4)
    lights = read_mem_bytes(SYM_LAST_LIGHTS, 1)[0]
    all_correct = (cfsr == 0 and sign == 1 and mag == 0x20 and rev == 1 and front == [5, 5, 5, 5] and lights == 1)
    check(all_correct,
          f"4 distinct CAN IDs back-to-back, all decoded correctly: CFSR=0x{cfsr:08X} sign={sign} mag=0x{mag:02X} rev={rev} front={front} lights={lights}",
          "expected CFSR=0, sign=1, mag=0x20, rev=1, front=[5,5,5,5], lights=1")

    # ---- 5. Gear field: ambiguous/malformed multi-bit pattern must not crash or misfire reverse ----
    print("\n>>> 5. Gear Field Robustness (CAN 0x1D0)")
    # 0x38 = Park per the confirmed mask -- must NOT be interpreted as reverse
    inject_can_frame(0x1D0, [0, 0, 0, 0x38, 0, 0, 0, 0])
    time.sleep(0.15)
    rev = read_mem_bytes(SYM_LAST_REVERSE, 1)[0]
    gear = read_mem_bytes(SYM_GEAR_FIELD, 1)[0]
    check(rev == 0 and gear == 0x38, f"Park gear correctly not treated as reverse: rev={rev}, gear=0x{gear:02X}", "expected rev=0, gear=0x38")

    # return to reverse to confirm edge-detection still works after the Park excursion
    inject_can_frame(0x1D0, [0, 0, 0, 0x20, 0, 0, 0, 0])
    time.sleep(0.15)
    rev = read_mem_bytes(SYM_LAST_REVERSE, 1)[0]
    check(rev == 1, f"Re-engaging reverse after Park correctly re-triggers: rev={rev}", "expected rev=1")
    # leave in a clean, safe state (out of reverse) for anyone using the board next
    inject_can_frame(0x1D0, [0, 0, 0, 0x10, 0, 0, 0, 0])
    time.sleep(0.15)

    # ---- 6. Final health check after the whole stress sequence ----
    print("\n>>> 6. Final Fault Check After Full Extended Sequence")
    cfsr = read_mem_words(REG_CFSR, 1)[0]
    hfsr = read_mem_words(REG_HFSR, 1)[0]
    check(cfsr == 0 and hfsr == 0, f"Zero faults after full extended input simulation: CFSR=0x{cfsr:08X} HFSR=0x{hfsr:08X}")

    print("\n" + "=" * 70)
    print(f" Extended Deep Test Summary: {passed}/{total} Tests Passed ({100.0*passed/total:.1f}%)")
    print("=" * 70)
    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
