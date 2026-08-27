# MCU Firmware (`can_app.bin`) — Verified Findings, Correcting Prior Handoff Docs

**Date**: 2026-08-27
**Method**: direct disassembly (`arm-linux-gnueabihf-objdump -b binary -m arm -M force-thumb`)
of the real `hardware/MCU/can_app.bin` (31,996 bytes, SHA-256
`1c21486a2a0bd969de9a9d98c902f1ca572206588d84a9faff377c56614b1c22`),
cross-checked byte-for-byte against the file on disk, not assumed from
prior docs.

## Why this doc exists

`docs/HANDOFF_MCU_AUDIO_I2C.md` and `docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`
cite specific addresses (a GPIO pin-function table, a `CMD 0xA0` jump
table at `0x080089D8`) for this same binary. Checking those addresses
directly against the real file, under an **incorrect load base
(`0x08000000`)**, initially made every citation look wrong or
out-of-bounds. The real load base — confirmed by the user, who sourced
the analysis — is **`0x08004000`** (the app loads above a 16KB
DFU/UART bootloader occupying `0x08000000`-`0x08003FFF`). Re-checked
under the correct base, several of the prior citations turn out to be
substantively real, though not always exactly as described. This doc
records what's actually verified, not what any prior doc claimed.

## Confirmed real: this is genuine STM32F1 GPIO driver code

Three shared low-level helper functions, called from many places
throughout the firmware with a runtime `(port_base, bitmask)` pair
rather than inlined literal peripheral addresses (which is *why* a
naive literal-address grep across the whole binary finds nothing — the
addresses are loaded once per port and passed as a register argument,
not re-embedded at every call site):

| Address | Behavior | Real STM32F1 register |
|---|---|---|
| `0x08005582` | `ldr r3,[r2,#8]; ands r3,r1; return (r3!=0)` | `IDR` (input data register), offset `+0x08` |
| `0x08005594` | `str r1,[r0,#20]` | `BRR` (bit reset register), offset `+0x14` — clears pins in mask, drives LOW |
| `0x08005598` | `str r1,[r0,#16]` | `BSRR` (bit set/reset register), offset `+0x10` — sets pins in mask, drives HIGH |

Confirmed via the literal pool: the port-base literal loaded before
one of these calls at `0x080059AC` is **`0x40010800`** — the real
STM32F1 **GPIOA** peripheral base address. This is unambiguous,
correctly-laid-out STM32F1 peripheral register access, not a
misattribution.

**Correction to my own earlier statement in this conversation**: I
previously reported zero references to STM32 GPIO peripheral addresses
anywhere in the binary. That was wrong on two counts: (1) it used the
incorrect `0x08000000` base, and (2) the search method itself was
flawed — it grepped for the literal's numeric *value* inside objdump's
`@ (0xADDR)` annotation, which only ever shows the literal pool's own
*address*, never its stored *value*. The value has to be read from the
file directly at that address, which is what the table above reflects.

## Confirmed real: `CMD 0xA0` dispatch is a genuine `TBB` jump table

At `0x080089D8` (verified in-bounds under the correct base):

```
80089d8: push {r4, lr}
80089da: ldr  r0, [pc, #436]      @ (0x8008b90)   ; received-frame struct pointer
80089dc: ldrb r0, [r0, #2]                          ; settingId = payload[0]
80089de: cmp  r0, #18
80089e0: bcs.n 0x8008ad0                            ; settingId >= 18 -> out-of-range handler
80089e2: tbb  [pc, r0]                              ; genuine Table Branch Byte
```

18 valid setting IDs (`0x00`-`0x11`). Full table decoded directly from
the raw bytes following the `tbb` instruction (not from objdump's own
mis-disassembly of the data region, which — like any linear
disassembler — tries to interpret table bytes as instructions and
produces garbage):

| settingId | Target | Notes |
|---|---|---|
| `0x00` | `0x080089F8` | multi-way branch on value (1/2/other), writes struct offset `0x3b` |
| `0x01`-`0x06` | `0x08008B88` | shared/no-op — same target as `0x0e`, likely "unimplemented" |
| `0x07` | `0x08008A40` | binary flag -> struct offset `0x3a` |
| `0x08` | `0x08008A5A` | binary flag -> struct offset `0x39` |
| **`0x09`** | `0x08008A74` | value(1/2) -> struct offset `0x38` — **confirmed = mic/audio input mux, the one already-shipped, working `custom_ui` feature** |
| `0x0a` | `0x08008A9E` | value (range-checked <10) -> struct offset `0x3c` |
| `0x0b` | `0x08008AB2` | writes offsets `0x3d`, `0x3(struct+3)`, `0x3e` (multi-field, likely a reset/init path) |
| `0x0c` | `0x08008B02` | halfword write to offset `0x40` (shifted value) + `0x3d`=1, `0x3e`=0 |
| `0x0d` | `0x08008B2A` | binary flag -> struct offset `0x42` |
| `0x0e` | `0x08008B88` | shared/no-op (same as `0x01`-`0x06`) |
| `0x0f` | `0x08008B46` | value -> struct offset `0x43` |
| `0x10` | `0x08008B52` | value -> struct offset `0x44` |
| `0x11` | `0x08008B5E` | value -> struct offset `0x45`; **the only handler that also calls out to a real function (`bl 0x080058A4`, r0=2 or 3)** rather than only writing state -- worth a closer look if anyone continues this |

None of the 18 handlers write GPIO registers directly. They all write
a byte into a shared in-RAM settings struct (base `0x08008B94`). A
*separate* set of periodic poll-loop sites reads each offset back out
and applies it to hardware — confirmed for the mic-mux path (offset
`0x38`): `0x08006B28`'s poll loop reads offset `0x38`, and on value `1`
calls `0x080085F0`, which itself calls into the `0x0800598C` chain
(uses the GPIO-read helper above with a `port_base` literal at
`0x08005998` and mask `1`) — a real, multi-layer state-machine, not a
direct register poke. Five more read-sites for the same offset exist
(`0x08009F00`, `0x0800A818`, `0x0800AA58`, `0x0800B4F2`, `0x0800B7DC`)
— likely one per related sub-feature (LED/relay/display feedback tied
to the same setting), not traced individually here.

## What this does NOT establish: no confirmed BD37033/audio-amp-power `CMD 0xA0` command

Checked all 18 real handlers' immediate bodies (see table above) for
anything that looks distinctly audio-amp/power-rail-related by its
struct-offset consumption pattern. None stand out as an obvious match
by inspection alone — but only the mic-mux offset (`0x38`) has been
traced all the way to its hardware-application poll site; the other 17
offsets (`0x39`-`0x45` etc.) have not been individually traced to
their own poll/apply functions. It's possible one of them is the real
audio-amp/BD37033-power path and just hasn't been chased down yet —
this would require repeating the same offset -> poll-site -> apply-
function -> GPIO-helper trace done here for `0x38`, for each remaining
offset. Real, bounded, mechanical work — not attempted for all 17 in
this pass given time spent, but the method above is proven and
repeatable.

**Practical recommendation unchanged**: the fastest way to actually
resolve whether BD37033's ~8V `VCC` rail is live right now is still a
direct multimeter measurement on the chip's own `VCC` pin, not further
firmware archaeology — this doc's value is in giving any future
firmware-side investigation a verified starting point (real load base,
real GPIO helper addresses, real decoded `CMD 0xA0` table) instead of
re-deriving or re-trusting the unverified prior table.

## Corrections to prior docs, stated plainly

- `docs/HANDOFF_MCU_AUDIO_I2C.md` / `docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s
  GPIO pin-function table (`PA_MUTE`, mic-mux, etc.) cites specific
  addresses under an apparent `0x08000000` load-base assumption that
  doesn't match this firmware's real `0x08004000` base. Some entries
  land near genuinely real code once corrected (e.g. `0x0800599C` is a
  real GPIOA-bit-8 accessor, though whether it's actually "Pin 1"/
  "PA_MUTE" as named, versus one of the several very similar nearby
  wrapper functions at `0x800599C`/`0x80059B0`/`0x80059D8` etc. --
  bit-8, bit-9, and further masks respectively -- was not resolved
  here); others (the `CMD 0xA0` jump table's cited address itself) were
  simply wrong under either base until corrected. Treat every specific
  address in those two docs as unverified until independently checked
  against the real file, same discipline applied in this doc.
