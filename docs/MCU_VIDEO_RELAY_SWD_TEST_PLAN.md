# MCU GPIO Direct-Manipulation Test Plan — SWD

**Status**: plan only, not yet executed. Written up for the next real hardware session.

## Why this test, and why it works despite RDP

RDP Level 1 protects **flash content** — it blocks external reads of flash via the
debugger, and (per this session's own BusFault finding) even blocks the CPU's own
native flash fetches while a debugger is attached. It does **not** restrict the
debugger's direct AHB-AP bus access to SRAM or peripheral registers. GPIO control
registers live in peripheral address space (`0x40010800`+), completely separate
from flash (`0x08000000`+) — the same mechanism already used successfully this
project (`mww 0xE0042004 0x300` for the DBGMCU watchdog freeze, `mdw 0x200001D8`
reading the live settings struct).

## Real constraint, established before writing this plan

Connecting SWD to this chip **always forces a reset**, regardless of `halt` vs
`reset halt` — an established, hardware-confirmed fact for this specific
setup, not something to second-guess. This means:

- We only ever get **bootloader-level context** once connected — no app code has
  run, so GPIO peripheral clocks are NOT enabled and none of the pins below are
  yet configured as outputs. We have to do the clock-enable and pin-mode
  configuration ourselves via `mww` before toggling BSRR/BRR does anything.
- The ArkMicro SoC's own reset line (GPIOB14) never gets released, so the SoC
  stays dead in reset for the entire test. **Every observation below is limited
  to whatever responds without the aftermarket SoC running** — the stock/OEM
  side of things (video, any OEM-side relay behavior, ambient audio), plus any
  purely mechanical/electrical response (relay click, LED, voltage change) a
  meter or ear can pick up independent of the SoC being alive.
- No flash is written, no RDP unlock/mass-erase command is issued, nothing
  outside SRAM/peripheral registers is touched. Zero risk to the resident
  bootloader or `can_app.bin`.
- **`GPIOB Pin 14` (the real SoC reset line, corrected this session) is
  deliberately EXCLUDED from this sweep.** Its function is already confirmed
  (hardware-confirmed twice over, independent of this plan) — toggling it here
  adds no new information and risks confusing the test session's own state
  (we already know forcing it doesn't actually complete a real boot sequence,
  since a bare level lacks whatever pulse/timing the real power-up sequencing
  might expect). Leave it untouched throughout.
- **`CAN1` (PA11/PA12) and the two UARTs (`USART2` PA2/PA3, `USART3` PB10/PB11)
  are NOT covered by this toggle-and-observe methodology** — they're
  communication peripherals, not discrete control signals, so "toggle and see
  what happens" doesn't apply the same way. CAN bus testing has its own,
  separate plan (see the earlier discussion on using a dev board as a passive
  CAN logger). Note also that the bootloader itself needs `USART2` for YMODEM,
  so those two pins may already be correctly clocked/configured by the
  resident bootloader by the time we connect — worth a quick `mdw` check on
  `GPIOA->CRL`/`CRH` in Phase 1 before assuming they need setup too, though this
  plan doesn't otherwise exercise them.

## Full pin list covered

| Pin | Claimed real function | Port reg base | BSRR/BRR bit | CRL/CRH nibble |
|---|---|---|---|---|
| PA1 | Audio Amp Mute | GPIOA (`0x40010800`) | `0x0002` | CRL bits `[7:4]` |
| PA15 | `id=0x0b` coordinated group (w/ PB8/PB9) | GPIOA | `0x8000` | CRH bits `[31:28]` |
| PB0 | Touch relay (OEM⟷SoC) | GPIOB (`0x40010C00`) | `0x0001` | CRL bits `[3:0]` |
| PB1 | `id=0x00`'s output | GPIOB | `0x0002` | CRL bits `[7:4]` |
| PB6 | Mic mux (OEM⟷SoC) | GPIOB | `0x0040` | CRL bits `[27:24]` |
| PB8 | `id=0x0b` group | GPIOB | `0x0100` | CRH bits `[3:0]` |
| PB9 | `id=0x0b` group | GPIOB | `0x0200` | CRH bits `[7:4]` |
| PC2 | Possible 2nd video-relay line | GPIOC (`0x40011000`) | `0x0004` | CRL bits `[11:8]` |
| PC13 | Video relay/mux (priority — see below) | GPIOC | `0x2000` | CRH bits `[23:20]` |

(`GPIOB Pin 14` intentionally excluded — see above.)

Port register offsets (same for all three ports): `CRL`=`+0x00`, `CRH`=`+0x04`,
`BSRR`=`+0x10`, `BRR`=`+0x14`. `RCC->APB2ENR` = `0x40021018`; `IOPAEN`=bit2
(`0x4`), `IOPBEN`=bit3 (`0x8`), `IOPCEN`=bit4 (`0x10`).

Push-pull output, 2MHz (`MODE=10, CNF=00`) nibble value = `0x2`, same as the
existing clean-room source's own convention for every one of these pins.

Using a **read-modify-write** pattern throughout rather than hardcoded
full-register values, since the exact power-on-reset state of unrelated bits
in these shared registers shouldn't be assumed or clobbered.

## Procedure

Run via OpenOCD telnet console (port 4444) or Tcl RPC, same session setup
already used successfully this project.

### Phase 1 — Connect and confirm bootloader-only context

```
reset halt
```

Sanity-check we're where we expect (deep in the bootloader, not the app):

```
reg pc
```

(Should NOT be anywhere near `0x08004xxx`+ — the app region. If it is, something
about the reset behavior changed since this plan was written; stop and
re-assess rather than continuing on a wrong assumption.)

Optional: `mdw 0x40010800` / `mdw 0x40010804` to check whether the bootloader
already configured `PA2`/`PA3` for `USART2` — informational only, doesn't
change anything below.

### Phase 2 — Enable GPIOA, GPIOB, GPIOC clocks

```
mdw 0x40021018
```

Note the value (`OLD_APB2ENR`). Compute `NEW_APB2ENR = OLD_APB2ENR | 0x1C`
(sets bits 2, 3, 4 — `IOPAEN`, `IOPBEN`, `IOPCEN` together), then:

```
mww 0x40021018 <NEW_APB2ENR>
```

### Phase 3 — Configure every pin in the table above as a push-pull output

Same read-modify-write pattern for each register touched. Six register writes
total (`GPIOA->CRL`, `GPIOA->CRH`, `GPIOB->CRL`, `GPIOB->CRH`, `GPIOC->CRL`,
`GPIOC->CRH`) — but each covers multiple pins in the table, so do them once each,
folding in *all* the pins that share that register before writing it back:

```
mdw 0x40010800   # GPIOA->CRL (need PA1 -> nibble [7:4])
```
`NEW = (OLD & ~(0xF<<4)) | (0x2<<4)`
```
mww 0x40010800 <NEW>
```

```
mdw 0x40010804   # GPIOA->CRH (need PA15 -> nibble [31:28])
```
`NEW = (OLD & ~(0xFU<<28)) | (0x2U<<28)`
```
mww 0x40010804 <NEW>
```

```
mdw 0x40010C00   # GPIOB->CRL (need PB0[3:0], PB1[7:4], PB6[27:24])
```
`NEW = (OLD & ~(0xF<<0) & ~(0xF<<4) & ~(0xF<<24)) | (0x2<<0) | (0x2<<4) | (0x2<<24)`
```
mww 0x40010C00 <NEW>
```

```
mdw 0x40010C04   # GPIOB->CRH (need PB8[3:0], PB9[7:4])
```
`NEW = (OLD & ~(0xF<<0) & ~(0xF<<4)) | (0x2<<0) | (0x2<<4)`
```
mww 0x40010C04 <NEW>
```

```
mdw 0x40011000   # GPIOC->CRL (need PC2[11:8])
```
`NEW = (OLD & ~(0xF<<8)) | (0x2<<8)`
```
mww 0x40011000 <NEW>
```

```
mdw 0x40011004   # GPIOC->CRH (need PC13[23:20])
```
`NEW = (OLD & ~(0xF<<20)) | (0x2<<20)`
```
mww 0x40011004 <NEW>
```

### Phase 4 — Baseline: drive everything LOW, record starting state

```
mww 0x40010814 0x8002    # GPIOA BRR: PA1, PA15 -> LOW
mww 0x40010C14 0x0343    # GPIOB BRR: PB0, PB1, PB6, PB8, PB9 -> LOW
mww 0x40011014 0x2004    # GPIOC BRR: PC2, PC13 -> LOW
```

Record, before touching anything further:
- Stock head unit screen state (expected: blanked, per the already-established finding).
- Any audible click/relay-actuation sound.
- Touchscreen response to a touch (if testable without SoC).
- Any audio present (ambient OEM radio, etc.) and its mute state.

This is the reference point every subsequent phase compares against.

### Phase 5 — PC13, priority pin (video relay)

```
mww 0x40011010 0x2000    # PC13 -> HIGH (BSRR)
```
Observe stock screen: comes back? stays blank? partial signal/no sync?
```
mww 0x40011014 0x2000    # PC13 -> LOW (BRR), back to baseline
```
Confirm it returns to the Phase 4 baseline (rules out one-way relay latching
being mistaken for level-triggered behavior).

### Phase 6 — PC2 (independently of PC13)

With PC13 held at whichever state showed *some* video in Phase 5 (or LOW if neither did):
```
mww 0x40011008 0x0004    # PC2 -> HIGH (BSRR)
```
Observe. Then:
```
mww 0x4001100C 0x0004    # PC2 -> LOW (BRR)
```
Observe again.

### Phase 7 — All 4 PC13×PC2 combinations, if either alone showed an effect

Run LOW/LOW, LOW/HIGH, HIGH/LOW, HIGH/HIGH and record each — resolves whether
PC2 is a second control line for the *same* relay versus unrelated.

### Phase 8 — PA1 (Audio Amp Mute)

```
mww 0x40010810 0x0002    # PA1 -> HIGH (BSRR) -- should MUTE if any audio is live
```
Listen. Then:
```
mww 0x40010814 0x0002    # PA1 -> LOW (BRR) -- should UNMUTE
```
Listen again. (Only informative if there's ambient audio to mute — e.g. OEM
radio playing independent of the dead SoC. If total silence either way, this
phase is inconclusive, not contradictory.)

### Phase 9 — PB0 (Touch Relay)

```
mww 0x40010C10 0x0001    # PB0 -> HIGH (BSRR)
```
Test touchscreen response (if any OEM touch functionality is independently
live). Then:
```
mww 0x40010C14 0x0001    # PB0 -> LOW (BRR)
```
Test again, compare.

### Phase 10 — PB6 (Mic Mux)

```
mww 0x40010C10 0x0040    # PB6 -> HIGH (BSRR)
```
```
mww 0x40010C14 0x0040    # PB6 -> LOW (BRR)
```
Hard to observe directly without an active mic/audio chain and something
recording both states — lower-value phase, include only if time allows.

### Phase 11 — PB1 (`id=0x00`'s output, function unconfirmed)

```
mww 0x40010C10 0x0002    # PB1 -> HIGH (BSRR)
```
Observe for ANY physical effect (screen, relay click, audio, touch, LED) —
this pin's real function is genuinely unknown, so this phase is exploratory.
```
mww 0x40010C14 0x0002    # PB1 -> LOW (BRR)
```

### Phase 12 — PA15/PB8/PB9 coordinated group (`id=0x0b`)

Real firmware fires these three together, so test them together rather than
individually:

```
mww 0x40010810 0x8000    # PA15 -> HIGH
mww 0x40010C10 0x0300    # PB8, PB9 -> HIGH (both at once)
```
Observe for any physical effect (this is the group the earlier finding
speculated might be a subsystem power-up sequence — watch/listen for
anything switching on).
```
mww 0x40010814 0x8000    # PA15 -> LOW
mww 0x40010C14 0x0300    # PB8, PB9 -> LOW
```

## Recovery

Standard, already-established recovery for this hardware: fully disconnect
OpenOCD, then a real **physical power cycle** of the head unit. Do not rely on
`resume` — per this session's own established finding, the CPU is trapped in
the bootloader with the application region unreachable while a debugger is
attached at all, so there is no live path back to normal operation without a
power cycle regardless of what this test does to any of the pins above.

## What this resolves, and what it still won't

**Resolves**: real, direct-observation confirmation of which pin states map to
which physical effects, for every pin in the table above that has an
observable consequence reachable without the ArkMicro SoC running — most
importantly the PC13/PC2 video-relay polarity question flagged in
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`.

**Does NOT resolve**: anything that requires the aftermarket SoC to be alive
(the CarPlay/AA display's own response, touch/mic routing that only matters
once the SoC is actually consuming those signals) — those need the
multimeter/logic-probe approach during real, non-SWD operation instead. Also
does not resolve the real firmware's own gating conditions (e.g. `flag_5e` for
`id=0x11`, or whatever sets `id=0x0b`'s struct offset) — this test bypasses
the firmware's logic entirely by driving the pins directly, which is the
point, but means it can't tell you when/why the real firmware would drive them
itself.
