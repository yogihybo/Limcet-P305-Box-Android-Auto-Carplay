#!/usr/bin/env python3
"""
Empirical, hardware-in-the-loop probe of the REAL Limcet MCU firmware's
SoC->MCU command dispatch, run against the physical STM32F105RBT6 spare
test board over SWD.

Follow-up to docs/MCU_LIMCET_DISPATCH_RECHECK_2026-09-15.md, which found
(via static disassembly only, NOT yet hardware-confirmed) that the real
firmware (live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin)
has only 5 SoC->MCU command handlers (0x81, 0x82, 0xA0, 0xFF, 0xE1) and
raised a lead -- not yet settled -- that the surviving command NUMBERS
may map to different handlers than the Volvo reference build's same
numbers (e.g. settings-sync-shaped code reachable via wire byte 0xFF
rather than 0xA0).

This script injects each of the 5 command bytes directly into the real
firmware's own UART RX ring structure in SRAM (bypassing the USART2 ISR's
byte framing) and observes GPIOA/B/C ODR + fault registers before/after,
to get real, empirical answers rather than relying on disassembly alone.

SRAM layout, derived via Capstone disassembly of the real firmware's own
USART2 IRQ handler (0x08008062) and its literal-pool loads -- NOT from a
source-built ELF/map (this is a reconstructed binary with no source):
  - g_rx_state   @ 0x20000058 (1 byte): 0=wait-sig, 1=cmd, 2=len, 3=payload
  - ring struct  @ 0x20000A21:
      byte[0]  = ring head/write index (0-7, wraps mod 8 -- confirmed via
                 the ISR's own completion code: increment, cmp #8, wrap)
      byte[4:] = 8 slots x 30 bytes each: {cmd(1), len(1), payload(28)}
                 (slot stride confirmed by the ISR's own idx*15*2 address
                 computation, and the len-bound check cmp r5,#0x1c=28)

RESULT (2026-09-15, run on real hardware): all 11 injected command/payload
combinations produced zero GPIO change and zero fault, using a properly
isolated baseline (a real, measured post-reset GPIO settling transient
was found and controlled for -- see docs/MCU_LIMCET_DISPATCH_RECHECK_2026-09-15.md's
"Empirical hardware test" section). This is INCONCLUSIVE, not a negative
result: follow-up tracing found the function this script's design had
assumed was "the ring consumer" (the one loading this table's own base
address as a literal) is actually something else entirely -- it indexes
the same table with stride 4 (a flat function-pointer array), not the
confirmed stride-8 (handler_ptr,cmd_byte) pair format, and can only ever
validly call index 0 (the CMD 0x81 handler) -- almost certainly an
unrelated mechanism (e.g. a startup-handshake retry) that happens to
reference the same table, not the USART2 RX ring's real consumer. The
real consumer was NOT reliably located in this pass. This script's
ring-write-and-advance-head technique may therefore simply not be
reaching the real dispatch at all -- treat the "no observable effect"
result as "inconclusive, real consumer not found," not as "these
commands are confirmed inert" or "the byte-remapping theory is
confirmed/refuted." See the recheck doc for the full, honest writeup.

Never touches the live vehicle unit. Spare STM32F105RBT6 board only, via
tools/pico_stm32.cfg. Pairs the real reconstructed app with this
project's own clean-room bootloader (which sets SCB->VTOR itself before
jumping, matching the combo already hardware-verified in
docs/BOOTLOADER_HANG_TRACE_2026-09-14.md section 26).
"""

import sys
import time

sys.path.insert(0, "tools")
from test_mcu_rigorous import run_ocd_commands, read_mem_bytes, read_mem_words

SYM_RX_STATE = 0x20000058
RING_BASE = 0x20000A21
RING_HEAD_OFF = 0x00
RING_SLOTS_OFF = 0x04
SLOT_STRIDE = 30
RING_SIZE = 8

GPIOA_ODR = 0x4001080C
GPIOB_ODR = 0x40010C0C
GPIOC_ODR = 0x4001100C
REG_CFSR = 0xE000ED28
REG_HFSR = 0xE000ED2C


def read_faults():
    return read_mem_words(REG_CFSR, 1)[0], read_mem_words(REG_HFSR, 1)[0]


def read_odrs():
    return {
        "A": read_mem_words(GPIOA_ODR, 1)[0],
        "B": read_mem_words(GPIOB_ODR, 1)[0],
        "C": read_mem_words(GPIOC_ODR, 1)[0],
    }


def read_ring_state():
    head = read_mem_bytes(RING_BASE + RING_HEAD_OFF, 1)[0]
    state = read_mem_bytes(SYM_RX_STATE, 1)[0]
    return head, state


def inject_raw(cmd, payload):
    """
    Writes a fully-formed ring slot at the current head index, then
    advances head (wrap mod 8) -- mirroring what the real ISR does on a
    successfully parsed frame, bypassing byte-by-byte framing entirely.
    """
    assert len(payload) <= 28
    head = read_mem_bytes(RING_BASE + RING_HEAD_OFF, 1)[0]
    slot_addr = RING_BASE + RING_SLOTS_OFF + head * SLOT_STRIDE
    frame = bytearray(30)
    frame[0] = cmd
    frame[1] = len(payload)
    for i, b in enumerate(payload):
        frame[2 + i] = b
    cmds = ["init", "halt"]
    for idx, b in enumerate(frame):
        cmds.append(f"mwb {slot_addr + idx:#x} {b:#x}")
    new_head = (head + 1) % RING_SIZE
    cmds.append(f"mwb {RING_BASE + RING_HEAD_OFF:#x} {new_head:#x}")
    cmds.append("resume")
    run_ocd_commands(cmds)
    return head, new_head, slot_addr


def probe(cmd, payload, label):
    run_ocd_commands(["init", "reset run", "exit"])
    # Let post-reset GPIO settling finish BEFORE taking the baseline --
    # confirmed via a direct control measurement that ODR keeps changing
    # on its own for close to 1s after reset (normal boot-sequence
    # settling, unrelated to any UART command), so a "before" read taken
    # too early falsely attributes that drift to the injected command.
    time.sleep(1.5)
    odr_before = read_odrs()
    head_before, state_before = read_ring_state()

    old_head, new_head, slot_addr = inject_raw(cmd, payload)
    time.sleep(0.4)

    odr_after = read_odrs()
    head_after, state_after = read_ring_state()
    cfsr, hfsr = read_faults()
    slot_readback = read_mem_bytes(slot_addr, 2 + len(payload))

    changed = {k for k in odr_before if odr_before[k] != odr_after[k]}
    print(f"\n>>> CMD {cmd:#04x} ({label}), payload={[hex(b) for b in payload]}")
    print(f"   ring: head {old_head}->{new_head} (readback now {head_after}), "
          f"rx_state {state_before}->{state_after}")
    print(f"   slot readback (should hold what we wrote): {[hex(b) for b in slot_readback]}")
    print(f"   CFSR={cfsr:#010x} HFSR={hfsr:#010x}")
    print(f"   GPIOA {odr_before['A']:#06x} -> {odr_after['A']:#06x}")
    print(f"   GPIOB {odr_before['B']:#06x} -> {odr_after['B']:#06x}")
    print(f"   GPIOC {odr_before['C']:#06x} -> {odr_after['C']:#06x}")
    if changed:
        print(f"   *** GPIO CHANGED on port(s): {sorted(changed)} ***")
    else:
        print(f"   (no GPIO ODR change observed)")
    return {
        "cmd": cmd, "changed_ports": changed, "cfsr": cfsr, "hfsr": hfsr,
        "head_before": old_head, "head_after": head_after,
    }


def main():
    print("=" * 72)
    print(" REAL Limcet Firmware -- Empirical SoC->MCU Command Dispatch Probe")
    print("=" * 72)

    run_ocd_commands(["init", "reset run", "exit"])
    time.sleep(0.3)
    cfsr, hfsr = read_faults()
    print(f"\nBaseline after fresh reset: CFSR={cfsr:#010x} HFSR={hfsr:#010x}")
    head, state = read_ring_state()
    print(f"Ring head={head}, rx_state={state}")
    odr = read_odrs()
    print(f"Baseline GPIO: A={odr['A']:#06x} B={odr['B']:#06x} C={odr['C']:#06x}")

    results = []

    # CMD 0x81 -- Volvo shape: init handshake / version broadcast (no payload)
    results.append(probe(0x81, [], "init-handshake shape"))

    # CMD 0x82 -- try both a Volvo-app-state-shaped payload (mode byte at
    # payload[0]) AND a Volvo-settings-shaped payload (setting_id/value at
    # payload[0]/[1]), since which shape (if either) this wire byte expects
    # in the real firmware is exactly the open question.
    results.append(probe(0x82, [0x01], "app-state shape (payload[0]=mode=1)"))
    results.append(probe(0x82, [0x00, 0x01], "settings-sync shape (id=0x00,val=1)"))

    # CMD 0xA0 -- same dual-shape probe, other direction
    results.append(probe(0xA0, [0x01], "app-state shape (payload[0]=mode=1)"))
    results.append(probe(0xA0, [0x00, 0x01], "settings-sync shape (id=0x00,val=1)"))
    results.append(probe(0xA0, [0x09, 0x01], "settings-sync shape (id=0x09 mic-mux,val=1)"))

    # CMD 0xFF -- same dual-shape probe: Volvo's own sub-id shape (0x7F),
    # AND the settings-sync shape (since the recheck doc's lead points here)
    results.append(probe(0xFF, [0x7F], "sub-id dispatch shape (0x7F)"))
    results.append(probe(0xFF, [0x00, 0x01], "settings-sync shape (id=0x00,val=1)"))
    results.append(probe(0xFF, [0x09, 0x01], "settings-sync shape (id=0x09 mic-mux,val=1)"))

    # CMD 0xE1 -- Volvo shape is "reboot to bootloader" (no payload needed);
    # the recheck doc's lead says this might actually be sub-id dispatch.
    # Send WITHOUT payload first (safe either way), and with the safe (non
    # sub-id-0x7F, i.e. no-op-if-sub-id-dispatch) probe value 0x01 second.
    # Deliberately never sends the real magic-cookie/direct trigger this
    # session traced (0x0800B842) -- that is a raw SRAM write this script
    # does NOT perform, and 0xE1's own real handler body (traced statically
    # to be the sub-id-checking shape, not a direct cookie write) makes
    # sending byte 0xE1 itself low-risk under either theory. Still: if
    # GPIO/ring state show something alarming, STOP and do not proceed to
    # try further payloads for this command.
    results.append(probe(0xE1, [], "no-payload probe"))
    results.append(probe(0xE1, [0x01], "sub-id dispatch shape (non-0x7F, safe no-op if this theory holds)"))

    print("\n" + "=" * 72)
    print(" Summary")
    print("=" * 72)
    any_fault = False
    for r in results:
        status = "FAULT" if (r["cfsr"] or r["hfsr"]) else "clean"
        gpio_note = f"GPIO changed: {sorted(r['changed_ports'])}" if r["changed_ports"] else "no GPIO change"
        head_note = f"head {r['head_before']}->{r['head_after']}"
        any_fault = any_fault or bool(r["cfsr"] or r["hfsr"])
        print(f"  CMD {r['cmd']:#04x}: {status}, {gpio_note}, {head_note}")

    # restore board to clean idle state
    run_ocd_commands(["init", "reset run", "exit"])
    time.sleep(0.2)
    cfsr, hfsr = read_faults()
    print(f"\nFinal state after reset: CFSR={cfsr:#010x} HFSR={hfsr:#010x}")

    return 1 if any_fault else 0


if __name__ == "__main__":
    sys.exit(main())
