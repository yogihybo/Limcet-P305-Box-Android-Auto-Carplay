#!/usr/bin/env python3
"""
Real-time CAN1 sniffer for the Prado companion STM32F105, driven entirely
over SWD via OpenOCD's Tcl RPC -- no application/bootloader code execution
involved. See docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md for the full rationale
and manual (mww/mdw) equivalent of every step this script automates.

Why this exists: manually typing individual `mdw` commands into the OpenOCD
telnet console cannot catch a momentary event (e.g. a quick steering-wheel
button press) -- each interactive round trip takes seconds. This script
polls CAN1's RX FIFO in a tight loop and logs every real frame with a
high-resolution timestamp, so events can be correlated against real-world
actions (button presses, gear changes, wheel turns) after the fact instead
of needing to catch them live.

Usage:
    python3 can_sniffer.py [--host localhost] [--port 6666] [--log FILE]

Requires OpenOCD already running with its Tcl RPC server reachable (same
setup already used successfully elsewhere in this project), and the target
connected via SWD (connecting forces a reset on this hardware -- expected,
not an error; we only ever get bootloader-level context, which is fine
since this script configures CAN1 itself and never executes app code).
"""

import argparse
import csv
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/..")
from openocd import OpenOcd  # tools/openocd.py

# --- Real register addresses, computed from hardware/MCU/source/include/stm32f105.h's
# own CAN_TypeDef layout -- see docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md for the
# full derivation of the sFilterRegister offset in particular. ---

RCC_APB1ENR = 0x4002101C
RCC_APB2ENR = 0x40021018
GPIOA_CRH   = 0x40010804
GPIOA_ODR   = 0x4001080C

DBGMCU_CR   = 0xE0042004  # already used successfully this project for the
                           # watchdog-freeze bits (DBG_IWDG_STOP/DBG_WWDG_STOP)

CAN1_BASE   = 0x40006400
CAN1_MCR    = CAN1_BASE + 0x00
CAN1_MSR    = CAN1_BASE + 0x04
CAN1_RF0R   = CAN1_BASE + 0x0C
CAN1_BTR    = CAN1_BASE + 0x1C
CAN1_RI0R   = CAN1_BASE + 0x1B0  # sFIFOMailBox[0].RIR
CAN1_RDT0R  = CAN1_BASE + 0x1B4  # sFIFOMailBox[0].RDTR
CAN1_RDL0R  = CAN1_BASE + 0x1B8  # sFIFOMailBox[0].RDLR
CAN1_RDH0R  = CAN1_BASE + 0x1BC  # sFIFOMailBox[0].RDHR
CAN1_FMR    = CAN1_BASE + 0x200
CAN1_FM1R   = CAN1_BASE + 0x204
CAN1_FS1R   = CAN1_BASE + 0x20C
CAN1_FFA1R  = CAN1_BASE + 0x214
CAN1_FA1R   = CAN1_BASE + 0x21C
CAN1_F0R1   = CAN1_BASE + 0x240  # sFilterRegister[0].FR1
CAN1_F0R2   = CAN1_BASE + 0x244  # sFilterRegister[0].FR2


def rmw(oocd, addr, set_mask=0, clear_mask=0):
    """Read-modify-write a single 32-bit register."""
    old = oocd.read_memory(addr, 1)[0]
    new = (old & ~clear_mask) | set_mask
    oocd.write_memory(addr, [new])
    return old, new


def init_can1(oocd):
    print("[*] Connecting and halting target (this forces a reset -- expected)...")
    oocd.send('reset halt')

    pc = oocd.read_register('pc')
    print(f"[*] Halted at PC=0x{pc:08x} (expect deep in the resident bootloader, "
          f"NOT the 0x08004xxx+ app region -- if this looks wrong, stop and "
          f"re-check rather than proceeding on a bad assumption)")

    # Real precaution, not assumed away: make sure CAN1 isn't frozen during
    # debug halt via DBGMCU_CR's DBG_CAN1_STOP bit (bit 14 on STM32F1
    # connectivity-line parts per general reference -- NOT independently
    # re-verified against a fetched ST datasheet this session, same honesty
    # caveat as everything else in this project sourced from general
    # knowledge rather than a confirmed primary source). Also (re)apply the
    # watchdog-freeze bits already used successfully elsewhere this project.
    old, new = rmw(oocd, DBGMCU_CR, set_mask=0x300, clear_mask=0x4000)
    print(f"[*] DBGMCU_CR: 0x{old:08x} -> 0x{new:08x} "
          f"(watchdog freeze on, CAN1 debug-freeze explicitly cleared)")

    print("[*] Enabling CAN1 + GPIOA + AFIO clocks...")
    rmw(oocd, RCC_APB1ENR, set_mask=(1 << 25))            # CAN1EN
    rmw(oocd, RCC_APB2ENR, set_mask=(1 << 0) | (1 << 2))  # AFIOEN, IOPAEN

    print("[*] Configuring PA11 (RX)/PA12 (TX)...")
    rmw(oocd, GPIOA_CRH, set_mask=(0x8 << 12) | (0xB << 16),
        clear_mask=(0xF << 12) | (0xF << 16))
    rmw(oocd, GPIOA_ODR, set_mask=(1 << 11))

    print("[*] Entering CAN1 initialization mode...")
    rmw(oocd, CAN1_MCR, set_mask=(1 << 0), clear_mask=(1 << 1))  # INRQ set, SLEEP clear
    for _ in range(100):
        if oocd.read_memory(CAN1_MSR, 1)[0] & 1:
            break
        time.sleep(0.01)
    else:
        print("[!] Timed out waiting for INAK -- CAN1 did not enter init mode")
        return False

    print("[*] Programming 500 kbit/s bit timing...")
    oocd.write_memory(CAN1_BTR, [0x004B0003])

    print("[*] Opening filter 0 to accept-all into FIFO0...")
    rmw(oocd, CAN1_FMR, set_mask=(1 << 0))                 # FINIT
    rmw(oocd, CAN1_FA1R, clear_mask=(1 << 0))               # deactivate filter 0
    rmw(oocd, CAN1_FS1R, set_mask=(1 << 0))                 # 32-bit scale
    rmw(oocd, CAN1_FM1R, clear_mask=(1 << 0))                # mask mode
    oocd.write_memory(CAN1_F0R1, [0x00000000])
    oocd.write_memory(CAN1_F0R2, [0x00000000])
    rmw(oocd, CAN1_FA1R, set_mask=(1 << 0))                  # re-activate filter 0
    rmw(oocd, CAN1_FMR, clear_mask=(1 << 0))                 # exit filter-init mode

    print("[*] Exiting CAN1 initialization mode (entering normal mode)...")
    rmw(oocd, CAN1_MCR, clear_mask=(1 << 0))                 # clear INRQ
    for _ in range(100):
        if (oocd.read_memory(CAN1_MSR, 1)[0] & 1) == 0:
            break
        time.sleep(0.01)
    else:
        print("[!] Timed out waiting to leave init mode")
        return False

    print("[*] CAN1 is live. Polling for real frames -- Ctrl+C to stop.\n")
    return True


def decode_frame(rir, rdtr, rdlr, rdhr):
    ide = (rir >> 2) & 1
    rtr = (rir >> 1) & 1
    if ide:
        can_id = (rir >> 3) & 0x1FFFFFFF
    else:
        can_id = (rir >> 21) & 0x7FF
    dlc = min(rdtr & 0xF, 8)  # DLC field is 4 bits (0-15) but real frames are
                               # capped at 8 bytes; clamp defensively rather
                               # than trust a value that shouldn't exceed 8
    data = bytearray(8)
    for i in range(4):
        data[i] = (rdlr >> (8 * i)) & 0xFF
    for i in range(4):
        data[4 + i] = (rdhr >> (8 * i)) & 0xFF
    return can_id, ide, rtr, dlc, bytes(data[:dlc])


def poll_loop(oocd, log_writer):
    frame_count = 0
    t0 = time.time()
    try:
        while True:
            rf0r = oocd.read_memory(CAN1_RF0R, 1)[0]
            pending = rf0r & 0x3
            if pending == 0:
                continue  # tightest possible loop; no sleep, we want max poll rate

            regs = oocd.read_memory(CAN1_RI0R, 4)  # RIR, RDTR, RDLR, RDHR (contiguous)
            rir, rdtr, rdlr, rdhr = regs
            can_id, ide, rtr, dlc, data = decode_frame(rir, rdtr, rdlr, rdhr)

            # Release the FIFO slot (RFOM0, bit 5 of RF0R) for the next frame --
            # read-modify-write via OR, matching can_driver.c's own convention
            # (CAN1->RF0R |= (1UL<<5)), not a blind overwrite.
            rmw(oocd, CAN1_RF0R, set_mask=(1 << 5))

            t = time.time() - t0
            id_str = f"{can_id:08X}" if ide else f"{can_id:03X}"
            data_str = data.hex().upper()
            frame_count += 1

            print(f"[{t:10.3f}] id={id_str}{'x' if ide else ' '} "
                  f"{'RTR' if rtr else 'DAT'} dlc={dlc} data={data_str}")
            if log_writer:
                log_writer.writerow([f"{t:.6f}", id_str, int(ide), int(rtr), dlc, data_str])

    except KeyboardInterrupt:
        print(f"\n[*] Stopped. Captured {frame_count} frames over "
              f"{time.time()-t0:.1f}s.")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', default='localhost')
    ap.add_argument('--port', type=int, default=6666)
    ap.add_argument('--log', default=None, help='CSV file to log every frame to')
    args = ap.parse_args()

    oocd = OpenOcd(args.host, args.port)
    try:
        oocd.connect()
    except Exception as e:
        sys.exit(f"Failed to connect to OpenOCD Tcl RPC at {args.host}:{args.port}: {e}")

    if not init_can1(oocd):
        sys.exit("CAN1 initialization failed -- see messages above")

    log_file = None
    log_writer = None
    if args.log:
        log_file = open(args.log, 'w', newline='')
        log_writer = csv.writer(log_file)
        log_writer.writerow(['t_seconds', 'can_id', 'ide', 'rtr', 'dlc', 'data_hex'])

    try:
        poll_loop(oocd, log_writer)
    finally:
        if log_file:
            log_file.close()
        oocd.close()


if __name__ == '__main__':
    main()
