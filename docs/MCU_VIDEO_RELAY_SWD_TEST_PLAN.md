# PC13/PC2 Video Relay Polarity Test — SWD Direct GPIO Manipulation

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

## Real constraint, corrected before writing this plan

Connecting SWD to this chip **always forces a reset**, regardless of `halt` vs
`reset halt` — an established, hardware-confirmed fact for this specific
setup, not something to second-guess. This means:

- We only ever get **bootloader-level context** once connected — no app code has
  run, so GPIO peripheral clocks are NOT enabled and PC13/PC2 are NOT yet
  configured as outputs. We have to do the clock-enable and pin-mode
  configuration ourselves via `mww` before toggling BSRR/BRR does anything.
- The ArkMicro SoC's own reset line (GPIOB14) never gets released, so the SoC
  stays dead in reset for the entire test. **This test can only observe the
  stock/OEM video path, not the aftermarket CarPlay/AA display** — that's
  expected and fine, since the open question is specifically about PC13/PC2
  polarity, and the stock feed alone is enough to answer it.
- No flash is written, no RDP unlock/mass-erase command is issued, nothing
  outside SRAM/peripheral registers is touched. Zero risk to the resident
  bootloader or `can_app.bin`.

## Real register addresses used

| Register | Address | Purpose |
|---|---|---|
| `RCC->APB2ENR` | `0x40021018` | bit 4 = `IOPCEN` (GPIOC peripheral clock enable) |
| `GPIOC->CRL` | `0x40011000` | pin config for PC0-PC7 (PC2 lives here) |
| `GPIOC->CRH` | `0x40011004` | pin config for PC8-PC15 (PC13 lives here) |
| `GPIOC->BSRR` | `0x40011010` | write 1 to a bit here to drive that pin HIGH |
| `GPIOC->BRR` | `0x40011014` | write 1 to a bit here to drive that pin LOW |

CRL/CRH are 4 bits per pin (`MODE[1:0]:CNF[1:0]`). Target value for push-pull
output, 2MHz (`MODE=10, CNF=00`) = `0x2`. PC13's nibble is bits `[23:20]` of
CRH (`(13-8)*4`); PC2's nibble is bits `[11:8]` of CRL (`2*4`).

Using a **read-modify-write** pattern rather than a hardcoded full-register
value, since the exact power-on-reset state of the other bits in these
registers shouldn't be assumed or clobbered.

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

### Phase 2 — Enable GPIOC clock

```
mdw 0x40021018
```

Note the value (`OLD_APB2ENR`). Compute `NEW_APB2ENR = OLD_APB2ENR | 0x10` (set
bit 4), then:

```
mww 0x40021018 <NEW_APB2ENR>
```

### Phase 3 — Configure PC13 and PC2 as push-pull outputs

```
mdw 0x40011004
```

Compute `NEW_CRH = (OLD_CRH & ~(0xF << 20)) | (0x2 << 20)`, then:

```
mww 0x40011004 <NEW_CRH>
```

```
mdw 0x40011000
```

Compute `NEW_CRL = (OLD_CRL & ~(0xF << 8)) | (0x2 << 8)`, then:

```
mww 0x40011000 <NEW_CRL>
```

### Phase 4 — Baseline: confirm both pins actually landed LOW

Power-on-reset default for the `ODR` bit under a fresh ` CRH`/`CRL` write is 0
(LOW) unless something already set it, but confirm rather than assume:

```
mww 0x40011014 0x2000    # PC13 -> LOW (BRR)
mww 0x4001100C 0x0004    # PC2  -> LOW (BRR)
```

Observe and record the stock head unit's screen state right now (expected:
blanked, matching the already-established finding — this is the baseline).

### Phase 5 — Toggle PC13, observe

```
mww 0x40011010 0x2000    # PC13 -> HIGH (BSRR)
```

Observe and record the stock screen. Does it come back? Stay blank? Something
else (e.g. static/garbage, suggesting partial signal but no sync)?

```
mww 0x40011014 0x2000    # PC13 -> LOW (BRR) -- back to baseline
```

Confirm it returns to the Phase 4 baseline state (rules out a one-way
relay-latching effect being mistaken for a level-triggered one).

### Phase 6 — Toggle PC2, observe (independently of PC13)

With PC13 held at whichever state showed *some* video in Phase 5 (or LOW if
neither state did):

```
mww 0x40011008 0x0004    # PC2 -> HIGH (BSRR)
```

Observe. Then:

```
mww 0x4001100C 0x0004    # PC2 -> LOW (BRR)
```

Observe again.

### Phase 7 — All 4 combinations, if Phase 5/6 individually showed any effect

If either pin alone visibly changed the stock feed, run all 4 combinations
(PC13×PC2 = LOW/LOW, LOW/HIGH, HIGH/LOW, HIGH/HIGH) and record the result for
each — this is what actually resolves whether PC2 is a second control line
for the *same* relay (e.g. a 2-bit select) versus an unrelated signal.

## Recovery

Standard, already-established recovery for this hardware: fully disconnect
OpenOCD, then a real **physical power cycle** of the head unit. Do not rely on
`resume` — per this session's own established finding, the CPU is trapped in
the bootloader with the application region unreachable while a debugger is
attached at all, so there is no live path back to normal operation without a
power cycle regardless of what this test does to PC13/PC2.

## What this resolves, and what it still won't

**Resolves**: real, direct-observation confirmation of which PC13 (and
possibly PC2) state maps to which physical video routing — settles the
"still open" polarity/topology question in
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` without relying on firmware
archaeology.

**Does NOT resolve**: whether the aftermarket CarPlay/AA path responds the
same way (untestable via SWD given the SoC-reset constraint above — would
need the multimeter/logic-probe approach during real, non-SWD operation
instead), or the real firmware's own `flag_5e` gating condition (this test
bypasses the firmware's logic entirely by driving the pins directly).
