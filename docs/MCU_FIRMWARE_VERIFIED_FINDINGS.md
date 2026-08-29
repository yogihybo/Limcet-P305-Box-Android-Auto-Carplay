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

## Follow-up: real port/pin identification for several more `CMD 0xA0` targets

Continued the same trace method (offset -> poll-site -> apply-function
-> GPIO helper literal) for the remaining settings. Port bases
resolved via the same literal-pool method (STM32F1: `GPIOA=0x40010800`,
`GPIOB=0x40010C00`, `GPIOC=0x40011000`):

| Function | Port.Pin | Driven by |
|---|---|---|
| `0x0800559C` | **GPIOB Pin 1** | `id=0x00`'s poll site (`0x08005E4C`, offset `0x3b==1`) |
| `0x08005654` | **GPIOA Pin 15** | multiple poll sites, incl. offsets `0x3b`, `0x3d`, `0x43`, `0x44` |
| `0x08005678` | **GPIOB Pin 9** | `id=0x0b`'s offset `0x3d==0` poll site (`0x08005DD0`) |
| `0x0800569C` | **GPIOB Pin 8** | same site as above |
| `0x0800598C` | **GPIOC Pin 0** | mic-mux (`id=0x09`) apply chain (read, not write) |
| `0x0800599C` | **GPIOA Pin 8** | a *read*, not a write -- unrelated to any mute function despite being the address the prior doc cited for `PA_MUTE` |
| `0x080059B0` | **GPIOC Pin 9** | unclear caller, not traced |

**Real correction**: `id=0x00`'s target is **`GPIOB Pin 1`**, not `GPIOA
Pin 1` as the prior doc claimed. The prior doc's cited address
(`0x0800599C`) is a real function, just not this one -- it's a GPIOA
Pin 8 *read*.

**One genuinely interesting, verified finding**: `id=0x0b` (struct
offset `0x3d`) drives a coordinated 3-pin enable when cleared to `0` --
`0x08005DD0` fires `GPIOA Pin 15`, `GPIOB Pin 9`, and `GPIOB Pin 8` HIGH
together in the same event, via `0x08005678`/`0x0800569C`/`0x08005654`
each called with arg `1`. That shape (multiple pins enabled together
in one code path) is more consistent with a subsystem power-up
sequence than a single-purpose signal like a buzzer beep -- but I
cannot confirm which physical board function `GPIOB Pin 8`/`Pin 9`
actually drive without a schematic or continuity test, so this is
flagged as a real, verified *code* finding, not a confirmed hardware
answer.

**`id=0x43`/`0x0f` and `id=0x44`/`0x10`** both funnel into
`0x08005654` (`GPIOA Pin 15`) under separate threshold-compare
conditions -- not simple direct pin sets, not obviously power-rail
related.

**`id=0x45`/`0x11`** selects between two states (`bl 0x080058A4` with
`r0=2` or `r0=3`) -- the only handler in the whole table that calls out
to a real function rather than only writing state, consistent with the
old table's "LCD Backlight PWM/Enable" claim for a nearby address,
though not independently confirmed here.

## Real physical falsification: no TDA7388 on this board

User confirmed via direct physical inspection: **no TDA7388 populated,
and no unpopulated footprint for one either** -- it's a large,
hard-to-miss package (Multiwatt/PowerSO), not something that could be
missed. This directly disproves the separately-pasted claim that
`PA_MUTE` targets "the TDA7388 speaker amp." Whatever `GPIOB Pin 1`
(the real target found above for `id=0x00`) actually drives, it isn't
muting a chip that doesn't exist on this board. Given BD37033 is the
only confirmed sound-processing chip physically present, it's more
plausible (not proven) that these MCU-controlled lines relate to it
directly rather than to a separate downstream amp stage -- but this
remains unconfirmed without a schematic or a working continuity test.

## Cross-check against `docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`: tabular data is genuinely reliable

That doc (a separate, more comprehensive Capstone-based pass over the
same `can_app.bin`) makes many more claims than the GPIO-pin table
already checked above -- a full interrupt vector table, three CAN
mode-dispatch tables, and a 9-entry inbound UART command table. Spot-
checking a table's correctness earlier in this doc doesn't imply the
rest is right, so each was independently re-verified byte-for-byte
against the real file rather than assumed:

- **Interrupt vector table** (§2, 13 entries: SP/Reset/NMI/HardFault/
  SVCall/SysTick/CAN1_RX0/USART1-3/UART4-5/CAN2_RX0): **13/13 exact
  matches**.
- **CAN mode-dispatch tables** (§4, three tables at `0x0800BB80`/
  `0x0800BB30`/`0x0800BAE8`, {can_id, handler} pairs, 9+10+9 entries):
  **28/28 exact matches**.
- **Inbound UART command table** (§5, `0x0800B9E4`, {cmd, handler}
  pairs): **9/9 exact matches** -- `0x81`, `0x82`, `0xA0`, `0xFF`,
  `0xE1`, `0x85`, `0x84`, `0x87`, `0x88`, all at the claimed handler
  addresses.

**Real conclusion: this document's tabular/structural extraction is
genuinely trustworthy** -- a different reliability class from the
GPIO-pin table already corrected above. The likely reason: fixed-
stride jump/dispatch tables are mechanical to extract correctly from a
disassembly pass, while per-pin GPIO tracing requires real data-flow
analysis (tracing a literal through register moves across function
calls), which is exactly where the earlier table went wrong. Table
*addresses* checking out doesn't guarantee every *semantic label* in
the same doc is correct (e.g. its own GPIO pinout table at the bottom
is the same one already corrected above) -- but the command/CAN-ID
tables themselves are real.

**New, real information this unlocks**: 5 previously-uncatalogued
inbound command bytes, all now confirmed real (not just described):
`0x85` (app-protocol response/ACK), `0x87` (Bluetooth AT command
relay), `0x88` (TEA cryptographic handshake for vendor auth -- 32
rounds against a fixed key, not a clock setter despite an early guess
elsewhere), `0xE1` (**reboot to bootloader** -- writes magic
`0x5555AAAA` to SRAM `0x20004004`, resets into YMODEM firmware-update
mode), `0xFF` (system state reset). Also 28 real vehicle CAN IDs
across 3 selectable dispatch modes (speed, HVAC, SWC, reverse gear,
door status, parking radar, etc.) -- real integration data, not yet
cross-checked against a live vehicle capture.

**Safety-relevant**: `tools/mcu-probe --sweep-cmds` now explicitly
skips `0xE1` by default (confirmed real reboot-to-bootloader, not
something a blind range sweep should ever hit) -- send it only
deliberately via `--send 0xe1`, with a real recovery plan (USB YMODEM
re-flash, `tools/mcu_builder/`) in hand first, not as part of general
probing.

## CRITICAL SAFETY FINDING (2026-08-28): connecting SWD holds the whole head unit in reset

Confirmed live, on real hardware, via a real OpenOCD/ST-Link SWD session
against the physical unit: **connecting a debugger to this MCU takes
down the entire head unit** -- display blanks, the ARK1668 SoC's own
console goes silent, everything. This was reproduced multiple times
across a live session (including after a full power cycle) and is not
an artifact of any specific command sequence -- it happens as soon as
SWD is connected. Full causal chain, each link independently verified
live tonight, not inferred:

1. OpenOCD's own connection/examine sequence briefly halts the core,
   even with no explicit `halt` ever issued -- standard behavior for
   establishing full debug control over an `hla_swd` (ST-Link)
   transport.
2. RDP Level 1 is active on this chip. Verified live via a precise
   BusFault trace (`CFSR=0x00008200` -> `BFARVALID`+`PRECISERR`,
   `BFAR=0x080004D4`, faulting `PC=0x080004AC`): RDP Level 1's real,
   documented behavior blocks the CPU's *own* flash reads -- not just
   external debug-port reads -- for as long as a debugger stays
   attached. This is a standing condition of being connected, not
   triggered by a specific command.
3. The instant the briefly-halted core is allowed to execute even one
   more instruction, it hits the first flash-literal read in its own
   boot code and BusFaults immediately -- reproduced live, same exact
   address, every single time (`0x080004AC` reading `0x080004D4`).
4. That fault happens extremely early -- inside the 16KB resident
   bootloader (`0x08000000`-`0x08003FFF`), well before the real
   application (`can_app.bin`, based at `0x08004000`) ever starts
   running. The core drops straight into its HardFault handler's
   infinite trap loop (`0x0800020E`/`0x08000210`, confirmed via
   single-step) and never gets any further.
5. **The application code that releases the SoC from reset --
   `GPIOB Pin 14` driven HIGH, see the section below -- lives in the
   application, not the bootloader, and only runs once, early in the
   application's own startup.** Since the fault happens before the
   bootloader ever hands off to the application, that release call
   never executes.
6. With no firmware ever configuring/driving that pin, it sits at its
   power-on-reset default (effectively low) -- holding the ARK1668 in
   reset for as long as the MCU stays trapped, i.e. for as long as the
   debugger stays connected.

**Practical implication for any future work on this platform: do not
connect a debugger to this MCU while the unit needs to stay usable.**
There is no known way to halt/examine the core without triggering this
chain -- it is not avoidable by using different OpenOCD commands, only
by not connecting at all. Recovery requires fully disconnecting SWD
(killing the OpenOCD process, ideally removing the physical SWD
connection) and a real power cycle -- a software-only reset does not
reliably clear residual `VTOR`/`NVIC` state left by any prior debug
activity (observed directly: a `reset halt` immediately before `resume`
still reproduced the identical fault on this session).

Also confirmed during this session: the SWD wiring in use has only
`SWDIO`/`SWCLK`/`GND`/`VCC` -- no `NRST` line -- so "connect under
reset" is not available as a recovery option; a genuine power cycle is
the only reliable fix if the connection itself becomes unresponsive
(e.g. after the target enters a low-power mode with no debugger
attached to catch it early).

## `GPIOB Pin 14` — real, verified ARK1668 SoC hardware reset control

Found while investigating the above. Real function at `0x08005A18`,
same shape as every other verified `SetPin(bool)` wrapper this session
(branches on the boolean parameter to the shared `BSRR`-set or
`BRR`-clear helper), bitmask `0x4000` (bit 14), port literal at
`0x08005A38` = **`0x40010C00` = GPIOB** (not GPIOA as the earlier,
already-corrected table claimed -- same class of port/pin error as
`id=0x00`'s target found earlier in this doc).

Traced its only two callers, both real: a mode-dispatch function at
`0x08007E7C` (parameter 0/1/2) calls the full ~9-function pin battery
including this one, with **opposite states between mode 1 (`state=1`,
HIGH) and mode 2 (`state=0`, LOW)** -- a deliberate, intentional flip
between two real system states, not incidental. That dispatch function
is itself called from a 5-step sequence state machine (`0x08004FD4`
region, driven by a state counter at SRAM `0x08005050`) and directly
from a standalone function at `0x08004DF8` which is called from
`0x08008348` -- a routine that also references the literal `0x5555AAAA`
and an infinite trap loop (`b.n` to self) immediately adjacent in the
disassembly, matching `CMD 0xE1`'s already-documented "reboot to
bootloader" behavior (writes that exact magic to SRAM `0x20004004`,
spins for a watchdog reset) almost exactly.

**Real interpretation**: this is not a signal requiring continuous
servicing during normal operation. It's one step in a deliberate,
multi-subsystem shutdown sequence -- called once to release the SoC
(mode 1, early in the MCU's own boot) and asserted again (mode 2) as
part of an orderly power-down or MCU-firmware-update
(`CMD 0xE1`) sequence, where holding the SoC in a known-safe reset
state makes real sense before the MCU either reboots into its update
bootloader or the whole system powers down. This is why halting the
core anywhere in the bootloader — before the application's one-time
release call ever runs — holds the SoC in reset indefinitely: it's not
that a continuous signal stopped, it's that a one-time startup action
never got the chance to happen at all.

## Future reference: a genuinely different RDP-Level-1 bypass worth trying (2026-08-28, not attempted)

Real, different technique found and worth revisiting if the bootloader
extraction question ever gets picked back up:
**https://github.com/racerxdl/stm32f0-pico-dump**

Unlike the `CVE-2020-8004` exception/PC-recovery technique already
ruled out above (which failed because this chip blocks the CPU's own
flash *execution* while a debugger is attached, not just external
debug-port reads), this tool exploits a **timing race condition in the
SWD protocol itself** -- sending minimal commands fast enough to read
data before the protection state catches up. Since it attacks the
debug-port protocol timing directly rather than relying on the CPU
executing/fetching flash, it's a genuinely different attack surface
than the one we already proved is blocked here.

**Real fit for our situation**: built specifically for **RDP Level 1**
(confirmed active on this chip, not Level 2 -- SWD access itself was
never blocked, only flash-content reads).

**Real requirements, not yet available in our setup**:
- A Raspberry Pi Pico (RP2040) -- cheap, not currently on hand.
- A controllable **`NRST`** line -- our current SWD wiring is
  `SWDIO`/`SWCLK`/`GND`/`VCC` only, confirmed no reset line (see the
  critical safety finding above for why this already mattered once
  tonight).
- A controllable **power** switch (relay/MOSFET) to the target,
  cycled rapidly and repeatedly *by the tool itself* as part of the
  attack -- not manual power cycling per attempt. Whether there's a
  realistic point on this board to add one without delicate soldering
  is unverified.

**Not attempted this session** -- recorded here so it doesn't need
rediscovering. Worth real consideration next time this is picked up,
if a Pico is on hand and a genuine `NRST` + switchable-power point can
be identified on the board.

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

## GPIOC Pin 13 (ArkMicro SoC reset line): real boot behavior confirmed
## different from the clean-room source's naive fixed-delay assumption

The clean-room `hardware/MCU/source/`'s claim that PC13 is the ArkMicro
SoC hardware-reset line, held LOW for a fixed 50ms then released, traces
back to a pasted "handoff" document from earlier in this project's
history -- **never independently re-derived from the real firmware in
this session until now.** Direct re-verification against the real
`can_app.bin`:

**Real boot-time behavior, traced end to end:**
- The real firmware's top-level init sequence (function at `0x080056C0`,
  called from what's very likely `main()` via a caller at `0x08006132`
  alongside CAN/UART/USART3 init calls) ends by calling a shared
  4-state dispatcher (`0x080058A4`) with `r0=0`, which resolves to
  **GPIOC Pin 13 = LOW, GPIOC Pin 2 = LOW**. So yes -- PC13 genuinely
  does get asserted LOW as part of real boot-time hardware init,
  consistent with "hold something in reset."
- **But there is no fixed-delay release anywhere in this sequence.**
  The very next call after the init routine returns is straight into
  the next subsystem init (`0x8007168`) -- no busy-wait loop, no
  immediate release-to-HIGH.
- The actual release-to-HIGH path is a **separate, main-loop-polled
  function** (branches back to `0x080084E6`, i.e. runs repeatedly, not
  once at boot) that debounces a status byte (bit 4 of a passed-in
  argument, source not traced) against a shadow copy, and only once
  that bit is confirmed changed/stable does it call the *same* shared
  dispatcher (`0x080058A4`) with `r0=2` or `r0=3` -- the HIGH-driving
  states for GPIOC Pin 13.
- **This is the exact same dispatcher `CMD 0xA0` id=0x11 also calls.**
  The earlier "collision" framing in this doc (id=0x11 vs. the SoC-reset
  pin) should be revised: it's not two unrelated features colliding on
  one pin, it's most likely **one coherent reset-release state machine**
  that both the main-loop poll and `CMD 0xA0` id=0x11 can drive --
  `id=0x11` may just be another entry point into the same subsystem
  (e.g. the SoC itself acknowledging/confirming release readiness over
  UART), not a separate LCD-backlight feature as originally guessed.
  This doesn't change the recommendation to leave the physical pin
  toggle unwired in `id=0x11`'s clean-room handler (see above) -- if
  anything it's a stronger reason to leave it alone until the real
  gating condition is understood, not a reason to wire it up.

**Practical conclusion for "can the SoC boot":** the real firmware does
NOT release the SoC from reset on a fixed timer -- it waits for a
polled condition. The clean-room source's fixed-50ms-then-release
approach is a real, meaningful simplification, not an exact match. It
was deliberately NOT changed to mimic the conditional/polled real
behavior, because that behavior's actual trigger (what sets bit 4 of
the polled status byte) was not traced far enough to reproduce
correctly, and a wrong conditional implementation risks the SoC never
being released at all (stuck in reset indefinitely) -- strictly worse
than the current fixed-delay approach, which is guaranteed to release
the SoC after 50ms regardless. **The clean-room source's SoC-boot path
is unchanged and, if anything, more conservative/safer than a
best-guess reproduction of the real conditional logic would be.**
Tracing the real gating condition (what feeds bit 4) is a real,
bounded follow-up if higher-fidelity behavior is ever needed.

## CORRECTION: the real SoC reset pin is GPIOB Pin 14, not GPIOC Pin 13

The clean-room source's "PC13 = ArkMicro SoC hardware reset" claim (and
everything built on top of it above, including the "id=0x11 collides
with the reset pin" caution) was wrong. It traced back to a pasted
handoff document, never independently re-derived from real disassembly
in this session until now.

**Real evidence, both independent of each other:**
- Direct disassembly: `0x08005A18` (port literal `0x40010C00` = GPIOB,
  mask `0x4000` = pin 14) is a real set/clear function reached from a
  real mode-dispatch function (`0x08007E7C`) -- genuinely GPIOB Pin 14,
  not GPIOC Pin 13.
- This project's own earlier **live hardware** finding (documented in
  the "CRITICAL SAFETY FINDING" section above, from real SWD sessions
  before this doc existed): connecting a debugger halts the CPU before
  a specific boot-time pin-release call runs, holding the whole ARK1668
  SoC in reset for as long as SWD stays connected. That release call
  was traced at the time to "GPIOB Pin 14 -> HIGH" via two real callers
  -- exactly matching the address found independently here.

Both point at the same real pin. GPIOC Pin 13 is a genuinely different
port/pin (`0x40011000`), with its own real behavior (boot-time LOW
default, released via a main-loop-polled condition, also reachable
from `CMD 0xA0 id=0x11`) -- most plausibly a camera/video relay
multiplexer per `docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s claim for
`id=0x11`, though that specific label is NOT re-confirmed to the same
standard as the reset-pin correction: that same doc's adjacent
`id=0x0d` "front camera auto-switch" claim was independently falsified
earlier in this doc (struct offset `0x42` has zero readers anywhere in
the firmware), so its GPIO semantic labels are not uniformly reliable
even when its addresses/tables are.

**Fixed in the clean-room source**: `main.c`'s `gpio_hardware_init()`
now holds/releases GPIOB Pin 14 (not PC13) for the SoC reset sequence.
`CMD 0xA0 id=0x11`'s handler now drives GPIOC Pin 13 directly (the
"collision" that justified leaving it unwired no longer applies) --
GPIOC Pin 2 remains unwired, since its own real trigger (a different
state of the shared dispatcher, reached from `id=0x00`'s `value==2`
branch) is a separate, unimplemented finding. Build-verified;
`can_app.bin` grows to 4064 bytes. Not written to physical hardware.

## Real, hardware-confirmed: the video relay theory is upgraded from plausible to confirmed

User-reported real hardware observation: with SWD connected (MCU halted,
same state as the "CRITICAL SAFETY FINDING" above), **the stock Toyota
head unit's own OEM video feed is completely blanked too**, not just the
aftermarket CarPlay/Android Auto display.

This is decisive in a way the earlier "aftermarket screen blank"
observation wasn't. The aftermarket screen going blank is already fully
explained by the ArkMicro SoC being held in hardware reset (GPIOB14 never
released while halted) -- nothing running, nothing to display, regardless
of any video mux. But the STOCK head unit's own feed going dark too can't
be explained that way: a properly-designed bypass relay should route the
factory camera straight to the factory screen without the aftermarket
SoC being alive at all. For the stock feed to also go dark specifically
while the MCU (not the SoC) is halted is real, direct evidence of a
physical video relay under **active** MCU control -- one that apparently
needs the MCU continuously driving it to maintain either video path,
rather than defaulting to a safe OEM-passthrough state when unpowered or
halted.

**Real product-safety implication, not just an RE curiosity**: if this
generalizes to any MCU fault (crash, lockup, brownout) during normal
driving, not just a deliberate SWD halt, it means the reversing camera
could black out entirely rather than fail safe to the stock OEM path.
Worth keeping in mind independent of the reverse-engineering work.

**Still open**: the exact polarity/topology. Which GPIOC13 state
(HIGH/LOW) maps to which physical routing, and whether GPIOC Pin 2 is a
second control line for the SAME relay (rather than an unrelated
function) are not resolved by this observation alone -- a real, bounded
follow-up via multimeter/logic-probe on the physical PC13/PC2 pins during
normal operation (reverse-gear engagement, etc.) would settle it more
precisely than firmware archaeology can.

## CMD 0x84 (Audio Route) real handler found -- GPIOC2 finally resolved, PA1's label falsified again

Prompted by a real, sound user question: "there must be an audio mute
function in the MCU, otherwise AA audio would play back through the
factory OEM speakers too." Checked directly rather than assumed --
disassembled the confirmed-real `CMD 0x84` handler (`0x08008808`) properly
for the first time this session (it had been glanced at once earlier, while
hunting for `CMD 0x87`, and mislabeled at the time as "unrelated code").

**Real findings, all independently verified:**
- `CMD 0x84` is NOT a simple PA1 mute toggle. That claim's originally-cited
  address (`0x0800599C`) was already proven wrong for this exact label
  earlier this session (it's a GPIOA-bit-8 *read*, unrelated) -- this
  closes the loop on that: the real `CMD 0x84` handler doesn't touch PA1 at
  all.
- The real handler masks the incoming value to 4 bits, ignores it if `>=6`,
  and stores it into a **debounced/shadowed state field in a different SRAM
  struct** (base `0x20000238`) than `CMD 0xA0`'s own struct (`0x200001D8`).
- `value==0` sends the literal ASCII string `"AT+AUDROUTE=1\r\n"` over
  USART3 -- the same channel `CMD 0x87`'s Bluetooth relay and `id=0x00`'s
  `"AT+UPGRADE"` command use -- then calls the exact same shared 4-state
  dispatcher (`0x080058A4`) `CMD 0xA0 id=0x11` calls, with state `0`.
  `value==3` sends `"AT+AUDROUTE=2\r\n"`, dispatcher state `1`. Values
  `1/2/4/5` just update the state field, no further action (real, not a
  gap).
- **This resolves GPIOC Pin 2**, flagged as unimplemented in the previous
  correction: full disassembly of the dispatcher's real truth table (traced
  `0x080058F8`/`0x0800591C` instruction-by-instruction) gives: state 0 =
  Pin13 LOW/Pin2 LOW, state 1 = Pin13 LOW/Pin2 HIGH, state 2 = Pin13
  LOW/Pin2 LOW (same physical result as state 0), state 3 = Pin13 HIGH/Pin2
  LOW.
- **Real, notable, unresolved finding**: this handler's own gate condition
  before calling the dispatcher ("proceed if this struct's own offset
  `0x5e` == 0") is the *opposite* polarity of `CMD 0xA0 id=0x11`'s gate
  ("proceed if its offset `0x5e` == 1") -- and given the two handlers use
  different struct bases, it's genuinely unclear whether these are the same
  underlying flag at different relative offsets into overlapping SRAM, or
  two independent flags. Not resolved this pass; implemented as two
  separate local state variables in the clean-room source rather than
  conflated.
- **Two real call sites sharing one relay pair is strong, independent
  support for GPIOC13/PC2 being a combined audio+video OEM-bypass relay**,
  not video-only as the earlier correction (based on `id=0x11` alone plus
  the decompilation doc's claim) had it -- audio routing driving the exact
  same physical pins as the video-relay setting is a much more specific,
  harder-to-coincidence signal than either finding alone.

**Fixed in the clean-room source**: `handle_audio_route()` (`CMD 0x84`)
rewritten to match the real logic exactly (debounced state, real
`AT+AUDROUTE=1/2` sends, dispatcher calls). New `shared_relay_dispatch()`
helper implements the full real 4-state truth table and is now used by
BOTH `CMD 0x84` and `CMD 0xA0 id=0x11` (previously id=0x11 only drove Pin
13 directly, leaving Pin 2 unwired). PA1's `main.c` comment downgraded from
a confident "Audio Amp Mute" label to explicitly unconfirmed -- kept as a
boot-time pop-prevention measure regardless, since that's sensible
independent of its true function, but no longer presented as a settled
label. Build-verified: `can_app.bin` grows to 4180 bytes. Not written to
physical hardware.

## Full CMD 0xA0 settings-list cross-reference: real SoC labels matched to real MCU behavior

Prompted by a real user question ("what does stock send for the MCU reverse
camera toggle") that led to finding the real, actually-active vendor class
for this hardware (`MCUAdapter_BoxP300`, selected by `McuType=6` in
`msnprofile/MsnProductInfo.ini` -- confirmed via `MCUAdapter::
getAdapterInstance()`'s real jump table, not assumed). An earlier symbol
search had landed on `McuAdapter_BoxP230` instead (a different, inactive
vendor box model this same shared library also supports) and produced a
real but irrelevant finding (a synthetic `CMD 0x91` key-press mechanism,
not in the MCU's own real command table at all) -- corrected before
drawing conclusions from the wrong class.

**Method**: `BoxP300::syncSettingDataToMcu(int itemIndex)` (`0x38df8`)
disassembled directly -- confirmed it sends genuine `CMD 0xA0
[itemIndex, value]`, the same mechanism this project's clean-room source
and `custom_ui` already use, with `itemIndex` becoming the real MCU
`settingId` byte-for-byte for every item except `11` (redirected to send
as `settingId 0x0c` instead of `0x0b`). Each item's real display label was
then recovered from `BoxP300::getSetItemText(int)` (`0x37e14`) by writing
a small ARM-condition-code simulator that walks the real binary-search-
style compare cascade for each concrete index value (catching a real bug
along the way: ARM lets multiple conditional branches share one `cmp`'s
flags, e.g. `cmp r8,#12; beq X; bgt Y` -- an initial naive scanner missed
the second branch and produced wrong results for most indices until
fixed), resolving each `QMetaObject::tr()` call's actual source-text
pointer via direct pool-literal + PC-relative address computation (no
GOT/relocation indirection needed for these -- confirmed the addressing
mode directly rather than assuming).

**Full real cross-reference** (SoC settings-list index -> label -> real MCU
`settingId` -> MCU's already-independently-confirmed real behavior):

| idx | Real label | -> settingId | MCU's confirmed real behavior |
|---|---|---|---|
| 0 | "Reversing camera" | `0x00` | GPIOB Pin 1, 4-way branch (value=2 sends real `AT+UPGRADE` over USART3) |
| 1-6 | *(empty string)* | `0x01-0x06` | **confirmed shared/no-op on the MCU side too** -- both ends agree independently |
| 7 | "Radar" | `0x07` | write-only, no consumer found anywhere in the firmware |
| 8 | "Trajectory" | `0x08` | write-only, no consumer found |
| 9 | "Reversing mode" | `0x09` | mic/audio input mux, GPIOB Pin 6 -- plausible real correspondence (mic behavior differing while reversing), not a mismatch |
| 10 | "360 camera" | `0x0a` | write-only, no consumer found (value transformed +1 before sending) |
| 11 | "Front camera" | `0x0c` (redirected from `0x0b`) | the real threshold gating the `id=0x0b` PA15/PB8/PB9 subsystem -- confirms that whole subsystem is genuinely camera-related, not the guessed-and-disproven `id=0x0d` |
| 12 | "Front camera time" | `0x0c` (same target as idx 11) | same threshold, different UI widget/value transform |
| 13 | "Speech button" | `0x0d` | write-only, no consumer found |
| 14 | "DVR" | `0x0e` | **confirmed genuine no-op** in the real firmware -- clean cross-confirmation |
| 15 | "Right Camera" | `0x0f` | plain stored value, confirmed no GPIO effect |
| 16 | "Left Camera" | `0x10` | plain stored value, confirmed no GPIO effect |
| **17** | **"Microphone"** | **`0x11`** | **GPIOC13/PC2 relay (gated by an internal flag)** |
| 18-21 | *(empty string)* | n/a | past the real list's populated range; also past the MCU's own real 18-entry dispatch limit (`settingId >= 0x12` is out-of-range there too) -- doubly closed |

**Two real, decisive corrections this resolves:**

1. **`id=0x11`'s real vendor-assigned meaning is "Microphone," not camera
   type.** Direct match, no redirect involved for index 17. Fully
   consistent with the independent finding (previous section) that
   `id=0x11` drives the exact same relay `CMD 0x84`'s real "Audio Route"
   drives -- both genuinely about audio routing. `custom_ui`'s "OEM Factory
   Camera" toggle sending `CMD 0xA0 id=0x11` was targeting the *microphone*
   setting, not a camera one -- it happened to share the same physical
   relay pin as the real audio route (which is presumably why the original,
   unverified assumption seemed plausible), but the real vendor software
   itself never uses `id=0x11` for camera switching. **Not yet fixed in
   source** -- documented here first, per explicit instruction to resolve
   the full picture before making changes.

2. **The real front/360-camera-related settings are `id=0x0a`/`id=0x0c`
   (and the `id=0x0b` group they gate)** -- not `id=0x0d`, which the
   original (unverified) decompilation doc guessed and this session already
   independently disproved (zero readers of that offset anywhere in the
   firmware). This cross-reference confirms *which* IDs the real "front
   camera auto-switch"-class feature actually lives at.

**Still genuinely unresolved**: no settings-list item (0-21) resolves to
`"AfterMarket Camera"`/`"Factory Camera"`/`"CameraType"` specifically --
those real strings exist in `libMcuCenter.so`'s `.rodata` (confirmed via
direct string search) but were not found referenced from this generic
settings-list mechanism via the addressing patterns checked. Either that
specific toggle isn't implemented via `CMD 0xA0` for the `BoxP300` box
variant at all (possibly a pure SoC-side video-source selection with no
MCU involvement), or it's referenced via a Thumb-mode code path or string-
construction idiom not covered by the ARM-mode pattern search used here --
genuinely open, not glossed over.

## RESOLVED: the real "camera" setting is a U-Boot env var, zero MCU involvement

Following up on "we know the camera setting works on stock, it must be
findable" -- searched the whole stock rootfs (not just `libMcuCenter.so`)
for the `"AfterMarket Camera"`/`"CameraType"` strings and found real hits
in `libSetting.so`, which has `FactoryWindow::on_btnCameraType_clicked()`
(`0x6703c`) -- the actual real button handler.

**Real, decisive finding: this is not a binary OEM/Aftermarket toggle at
all.** It's a 7-option reverse-camera **video format** selector, presented
via a real `OptionListDialog`. The option strings, resolved in program
order from the real binary:

| idx | Label |
|---|---|
| 0 | Auto |
| 1 | CVBS-PAL |
| 2 | CVBS-NTSC |
| 3 | AHD 720P 25FPS |
| 4 | AHD 720P 30FPS |
| 5 | AHD 1080P 25FPS |
| 6 | AHD 1080P 30FPS |

This matches, index-for-index, the real kernel driver's own
`enum carback_camera_mode` (`linux-arkmicro/linux/drivers/media/i2c/
rn6752.c`: `DYNAMIC, CVBS_PAL, CVBS_NTST, 720P25, 720P30, 1080P25,
1080P30`) -- confirming this selects the reversing camera's real video
signal format/protocol (OEM Toyota cameras and generic aftermarket AHD
cameras genuinely use different signal formats), not an MCU-mediated
relay choice.

**Real apply mechanism, both confirmed via direct string/call-site
resolution -- zero MCU/UART/CAN involvement:**
1. `sprintf("fw_setenv carback_camera_mode %d", selectedIndex); system(...)`
   -- persists the choice as a **U-Boot environment variable**, read by
   `linux-arkmicro/u-boot/board/arkmicro/ark1668_limcet_p305/
   ark1668_display_cfg.c`'s `ark_carback_camera_check()` (already known to
   this project -- it's the function that gates U-Boot's own instant
   reverse-camera preview) for the next boot.
2. `sprintf("echo \"camera_mode %d\" > /sys/devices/platform/i2c-gpio.1/
   i2c-1/1-002c/dvr", selectedIndex); system(...)` -- applies it
   immediately at runtime via a sysfs attribute the `rn6752` kernel driver
   exposes on the real video-decoder chip (I2C address `0x2c` on
   `i2c-gpio.1`/`i2c-1`, the same device address a nearby, separately-found
   `cat .../support_max_resolution` capability query also targets).

**This means the `"AfterMarket Camera"`/`"Factory Camera"`/`"CameraType"`
strings found earlier in `libMcuCenter.so` belong to something else
entirely** -- unrelated dead code, a different vendor box variant sharing
the same compiled library, or a genuinely different (never found) UI
element -- not this real, active, stock-confirmed camera-format feature.
`custom_ui`'s existing "OEM Factory Camera" binary toggle (sending
`CMD 0xA0 id=0x11`) was built on a wrong premise from the start: the real
stock feature is a 7-way format picker with zero MCU involvement, not a
binary relay switch.

**Fix applied**: `custom_ui`'s camera-format setting now replicates the
real stock mechanism directly (`fw_setenv` + the same sysfs write), instead
of any `CMD 0xA0`/`CMD 0x84` MCU command. `CMD 0xA0 id=0x11` is no longer
sent for camera purposes at all -- freed up for its real, vendor-confirmed
meaning (the previous section: "Microphone"), now wired to a genuinely
separate OEM/Aftermarket microphone toggle instead.

**CORRECTION (2026-08-29, same day)**: the above camera-format finding is
real and correct, but it is NOT the feature the user actually meant by
"the camera choice -- stock OEM or aftermarket -- which controls if the
video multiplexer reverts to OEM or stays with aftermarket feed." That is
a genuinely different, still-real stock feature, found below. The
7-format `carback_camera_mode` picker stays implemented (it's real, it
just isn't this).

## The REAL OEM/Aftermarket relay toggle: `CanBus_Raise_Toyota::enableOEMSound(bool)`

Per the user's own hint earlier this session ("the device has both options
in the setting menu" -- CAN bus and MCU), searched `usr/lib/libCanBus.so`
directly for the `"AfterMarket Camera"`/`"OE Camera"` strings and found
real hits, landing in `CanBus_Raise_Toyota::getSetItemValueTexts()` --
a Toyota-specific adapter class in a parallel "CanBus" vendor plugin
family (`CanBusAdapter::getAdapterInstance(CanBusType)`, alongside
`MCUAdapter::getAdapterInstance(McuType)` -- two separate, real vendor
selection mechanisms, matching the user's hint literally).

**Real, dedicated function found**: `CanBus_Raise_Toyota::enableOEMSound(bool)`
(`0x78a60`) -- disassembled directly, not guessed. Real, decisive
findings:

- **It sends real `CMD 0x84` frames using the exact same `0x2E`-signature
  wire protocol** already fully reverse-engineered this session
  (`makeCanBusProtocol` writes byte `0x2E` as the first frame byte,
  identical structure to `MCUAdapter_BoxP300`'s `makeMCUProtocol`) --
  confirmed by direct disassembly of the frame-builder, not inferred from
  the function name alone.
- **Same physical port**: `CanBusAdapter::getPortName()` reads a
  `"CANPortName"` config key with default value `"/dev/ttyHS0"` -- the
  identical UART device the companion MCU is connected to. This "CanBus"
  mechanism is not a separate physical CAN interface; it's a second
  command family riding the same wire as everything else.
- **Real byte sequences, read directly from the disassembly**:
  - `enableOEMSound(false)`: one frame, `CMD 0x84`, payload `[0x00, 0x00]`.
  - `enableOEMSound(true)`: three frames, `CMD 0x84`, payloads
    `[0x08, 0x01]`, `[0x09, 0x01]`, `[0x2A, 0x01]`.

**Real, unresolved discrepancy, stated plainly**: this session's own
earlier disassembly of the MCU's real `CMD 0x84` handler (`0x08008808`)
reads its "value" byte from frame offset `+3`. Under this project's own
already-established frame-layout convention (`frame+2` = `payload[0]`,
confirmed extensively for `CMD 0xA0` and others), `frame+3` would be
`payload[1]` -- meaning the real handler's dispatch value is
`payload[1]`, not `payload[0]` as this session's clean-room
`handle_audio_route()` currently implements. Under that reading, all
three of `enableOEMSound(true)`'s frames carry `payload[1]=1`, which this
project's own established `CMD 0x84` value table calls a no-op (only
`value==0`/`value==3` do anything) -- while `enableOEMSound(false)`'s
`payload[1]=0` would trigger the real `"AT+AUDROUTE=1"` + relay-state-0
action. This is a real, non-obvious inconsistency (why would `false`
trigger a real action and `true` not?) that was NOT resolved this pass --
possible explanations include a wrong offset assumption for this specific
command's frame-passing convention, `payload[0]` (the "sub-id" values 8/9/
42) mattering rather than `payload[1]`, or the true/false semantics being
inverted from what's assumed here. Not chased further given real effort
already spent; flagged honestly rather than picking an explanation to
sound resolved.

**Practical choice made despite the open question**: implemented by
replicating stock's exact real byte sequences verbatim (three `CMD 0x84`
frames for "OEM", one for "Aftermarket", exact payloads above) rather than
trying to independently re-derive the semantics -- this is the safest,
most hardware-compatible approach precisely because it doesn't depend on
resolving the payload-offset question correctly; whatever the real MCU
firmware actually does with these exact bytes, sending the exact bytes
stock sends is the best available proxy for "do what stock does."

---

## RETRACTION (2026-08-29): `CanBus_Raise_Toyota::enableOEMSound` is dead code on this hardware

The user's own real hands-on observation prompted a direct re-check that
overturns the finding above: **the stock factory-menu traffic logger
shows MCU command traffic but never any CAN-bus traffic**, on real
hardware. That observation is now confirmed correct by static analysis,
not just plausible:

- **`usr/lib/libCanBus.so` is never loaded by anything on this rootfs.**
  A whole-rootfs string search for the literal substring `"libCanBus"` --
  the only way any binary could `dlopen()` it by name -- returned **zero
  hits**, anywhere: not in `MsnCoreApp`, not in `libSetting.so`, not in
  `libMcuCenter.so`, not in any other `.so`. `readelf -d` confirms it's
  also not a direct `NEEDED` dependency of any of those. The file exists
  on disk (probably shipped as part of a shared vendor SDK image used
  across multiple products) but nothing on this specific build ever
  instantiates `CanBusAdapter::getAdapterInstance()` or reaches
  `CanBus_Raise_Toyota` at all. **The whole `CanBus_*` adapter family
  is orphaned dead weight here, not a live, gated-off code path.**
- This means `enableOEMSound(bool)`'s `CMD 0x84` frames, verbatim-copied
  into `custom_ui`'s `hal::sync_video_relay()`, were never actually
  stock's real mechanism for this device -- they're real bytes from a
  real function, but a function stock itself never calls on this unit.
  Sending them may do *something* (the MCU's own `CMD 0x84` handler will
  still process whatever bytes arrive), but there is no vendor-side
  confirmation this is the correct OEM/Aftermarket toggle for this
  hardware. **Treat `sync_video_relay()`/`send_mcu_video_relay()` as
  unconfirmed, matching the same caution this project already applied to
  the earlier wrong `McuAdapter_BoxP230` lead** -- not reverted outright
  (the bytes are harmless to send, and might still coincidentally be
  correct if a Toyota-specific CAN feature was cloned across
  `libMcuCenter.so`'s own settings table, see below), but not to be
  trusted as "the real fix" either.

### The real, live lead: `libMcuCenter.so`'s own internal settings-name string table

While confirming `libCanBus.so` was dead, the exact same strings
(`"AfterMarket Camera"`, `"OE Camera"`, `"OEM Camera"`) turned up **again**
inside `libMcuCenter.so` itself -- the confirmed-live library
`MCUAdapter_BoxP300` lives in. This is a different, real string cluster,
not a coincidence: a contiguous block of `.rodata` (file offset
`~0xb2c88`-`~0xb2dec`) reading, in order:

```
AfterMarket Camera   Factory Camera
AfterMarket 360       Factory 360
CAN Active            12V Active
P Key Active           Radar Active
10 s / 15 s
OEM Microphone         AfterMarket Microphone
Reversing camera       Trajectory
Reversing mode         360 camera
Front camera           Front camera time
Speech button
Right Camera           Left Camera
BT Pin Code            Car Setting          Auto Meter
```

This is unmistakably a genuine vendor settings-label table -- paired
Factory/AfterMarket labels for Camera, 360, and Microphone, sitting right
alongside real hardware config concepts (`CAN Active`/`12V Active`/
`P Key Active`/`Radar Active`, `WhellDelay`, `SettingItemTypes`) that are
clearly `libMcuCenter.so`'s own internal concern, not something borrowed
from the dead `libCanBus.so`. **This -- not `CanBus_Raise_Toyota` -- is
the real, live location of the "OEM Factory Camera" vs "AfterMarket
Camera" toggle for this device.** `nm -CD` shows no exported symbol
directly implicated (no `getName`/`getItemName`/`SettingItemTypes`
function is exported by name -- likely inlined or resolved through a
private/static helper), and no `MCUAdapter_BoxP307` class exists (`Box-P307`
sitting nearby is a `LauncherName`/`ProductNumber` string value, not a
class name) -- so the exact function that indexes into this table and the
exact `CMD`/setting-id it drives are **not yet pinned down**. This is the
correct next step, not `CanBus_Raise_Toyota`.

**Also newly confirmed real** in this same string cluster:
`"OEM Microphone"` / `"AfterMarket Microphone"` as a genuine adjacent
pair -- corroborates this session's earlier, separate finding that
`CMD 0xA0 id=0x11` is real vendor-labeled "Microphone" (not
camera-related), now from a second, independent source (a raw string
table, not just the `getAdapterInstance`/settings-list cross-reference
done earlier).

**Also still unconfirmed**: whether `CanBus_Raise_Toyota` (as opposed to
`MCUAdapter_BoxP300`, the class independently confirmed active via
`McuType=6`) is genuinely the active/relevant class for this specific
hardware, or another compiled-in-but-inactive vendor variant (the same
class of caution that ruled out `McuAdapter_BoxP230` earlier this
session) -- no equivalent "CanBusType" config key confirming this was
found in `msnprofile/*.ini`. Given the real, decisive semantic match
(dedicated Toyota-specific "OEM sound" enable function, same protocol,
same port) this is treated as the best available real lead regardless,
but real-hardware verification (`tools/mcu-probe`, watching both screens)
is the way to actually confirm it does something.

---

## CONFIRMED (2026-08-29): the real Camera Type setting -- `CMD 0xA0 id=0x01`

Traced hard, per explicit direction, starting from the `libMcuCenter.so`
string-table lead in the RETRACTION section above. This is now a full,
byte-exact, disassembly-confirmed finding, not a name-string coincidence.

### Step 1: locating the real `getSetItemValueTexts(int)` implementation

Every `MCUAdapter_Box*`/`MCUAdapter_Msn*`/`MCUAdapter_ZhongHang` sibling
class in `libMcuCenter.so` overrides `getSetItemValueTexts()`, and several
(`BoxP300`, `BoxP900`, `MsnDecoder`, `McuAdapter_BoxP230`, `BoxC270`,
`BoxC280`, `ZhongHang`) reference the exact same `"AfterMarket Camera"`/
`"OE Camera"`/`"AfterMarket 360"` string cluster -- confirming this is a
shared vendor concept across the whole product family, not one-off.
Traced the **confirmed-active** class specifically:
`MCUAdapter_BoxP300::getSetItemValueTexts(int)` (`0x36750`).

### Step 2: the jump table, walked precisely (not eyeballed)

The function does `cmp r8, #17; addls pc, pc, r8, lsl #2` (r8 = the real
`int idx` argument, passed via r2 under this function's sret-return ABI)
-- an 18-entry jump table, indices 0-17. Walked every entry
programmatically (parsed the real `objdump` listing, resolved every
`ldr rX,[pc,#N]` + `add rX,pc,rX` PC-relative string-load pair inside each
handler's real address range) rather than reading table entries by eye,
which is how the earlier RETRACTION section's off-by-one misread (which
looked like idx1 was the default case) got caught and corrected. Real,
complete result:

| idx | target | value texts (append order = value order) |
|---|---|---|
| 0 | `0x36910` | `OEM Microphone`, `AfterMarket Microphone` |
| 1 | `0x369e0` | `AfterMarket Camera`, `Factory Camera`, `AfterMarket 360`, `Factory 360` |
| 2-7 | `0x36910` | (share idx0's handler -- effectively unused/aliased slots) |
| 8 | `0x36b54` | `Off`, `On` |
| 9 | `0x36c18` | `Off`, `On` |
| 10 | `0x36cdc` | `CAN Active`, `12V Active`, `P Key Active` |
| 11 | `0x3707c` | (empty -- no fixed strings, dynamic/generic case) |
| 12 | `0x36ebc` | `Off`, `Radar Active`, `5 s`, `10 s`, `15 s` |
| 13 | `0x371a4` | `5 s`, `10 s`, `15 s` |
| 14 | `0x36df8` | `Off`, `On` |
| 15 | `0x370e0` | `Off`, `On` |
| 16 | `0x367e8` | `Off`, `On`, `12V Active` |
| 17 | `0x372ac` | `Off`, `On`, `12V Active` |

**idx1 is unambiguously the Camera Type setting**: exactly 4 value texts,
in order, matching a real 4-way vendor combobox (single reversing camera
AfterMarket/Factory paired with a 360-camera-system AfterMarket/Factory).

### Step 3: the real send function -- confirms idx IS the wire id, unmodified for idx=1

`MCUAdapter_BoxP300::syncSettingDataToMcu(int)` (`0x38df8`) is the real
"push a setting to the MCU" function (reads the current value via
`MsnIniConfig::value()`, decides what to send). Disassembled its full
value-dispatch logic (`0x38f18`-`0x39018`) -- **not inferred, read
directly**:

```
cmp r4, #12    beq -> remap: id stays 12, value = ((old_value+1)*5) & 0xFF
cmp r4, #11    beq -> if current_value_index > 1: id becomes 12 (remapped!)
                      else: id stays 11 unmodified
cmp r4, #10    beq -> id stays 10, value = old_value + 1
default:              lr = (uint8_t)idx   <-- id = idx, UNMODIFIED
```

For every idx except 10/11/12 (which get special value-remapping, not
relevant to idx=1), the default path fires:

```
   38f34: uxtb lr, r4       ; lr = (uint8_t)idx        -- becomes payload[0]
   38f38: mov  r3, #2       ; len = 2
   38f4c: mov  r2, #160     ; r2 = 0xA0                -- CMD byte, CONFIRMED
   38f54: strb lr, [sp,#28] ; payload[0] = idx (unmodified for idx=1)
   38f58: strb ip, [sp,#29] ; payload[1] = current/target value index
   38f5c: bl   makeMCUProtocol
```

**This directly, byte-exactly confirms: `CMD 0xA0`, setting id `0x01`
(idx=1 sent unmodified), value 0-3 selecting AfterMarket Camera / Factory
Camera / AfterMarket 360 / Factory 360.** Not a name-string coincidence --
traced through the real send path end to end.

**Implemented**: `hal::sync_video_relay(bool oem)`
(`custom_ui/src/hal/mcu_input.cpp`) now sends `CMD 0xA0` with
`payload = [0x01, oem ? 0x01 : 0x00]` -- replacing the retracted
`CanBus_Raise_Toyota`-based implementation entirely (not just
downgrading the doc comment this time). `settings_screen.cpp`'s "OEM
Factory Camera" toggle is unchanged at the call-site level (still calls
`hal::send_mcu_video_relay(oem)`), only its own doc comment updated.
**Not yet hardware-tested** -- needs a real device run, watching whether
the video multiplexer actually switches feeds.

### Related, unresolved discrepancy found during this trace (flagged, not yet fixed)

The **same rigorous idx-walk above directly contradicts this session's
earlier, less rigorous "`CMD 0xA0 id=0x11` = Microphone" finding**, which
the already-shipped "OEM Microphone Relay" toggle in `settings_screen.cpp`
relies on (`hal::send_mcu_setting(0x11, ...)`). Per the table above:

- **idx0 (wire id `0x00`, unmodified -- not a special-cased id) is the
  real Microphone setting**: `getSetItemValueTexts(0)` returns exactly
  `"OEM Microphone"` / `"AfterMarket Microphone"`, a genuine OEM/
  AfterMarket value pair -- not a generic on/off.
- **idx17 (wire id `0x11`) returns `"Off"`/`"On"`/`"12V Active"`** -- an
  unrelated 3-way setting, nothing to do with a microphone.

This means the existing "OEM Microphone Relay" toggle (added earlier this
session, `id=0x11`) is very likely wired to the wrong setting id, and the
real OEM/AfterMarket microphone toggle is probably `CMD 0xA0 id=0x00`
instead -- **not fixed in this pass** (out of scope for the specific
camera-relay trace just requested), but flagged clearly so it isn't lost.
Real next step if this is picked up: re-verify against
`MCUAdapter_BoxP300::syncSettingDataToMcu`'s value-remap logic (id=0x00
isn't one of the 10/11/12 special cases, so it should also be a clean,
unmodified `payload[0]=0x00` send) before changing the toggle.

---

## FIXED (2026-08-29): the real Microphone setting corrected to `CMD 0xA0 id=0x00`

Follow-up to the "related, unresolved discrepancy" flagged above -- fixed
per explicit request. `settings_screen.cpp`'s "OEM Microphone Relay"
toggle now sends `CMD 0xA0 [0x00, value]` instead of the wrong
`[0x11, value]`.

**Value polarity, confirmed from the real append order** (not assumed
symmetric with the Camera Type setting, which has opposite polarity):
`getSetItemValueTexts(0)` appends `"OEM Microphone"` first, then
`"AfterMarket Microphone"` -- so **value 0 = OEM, value 1 = AfterMarket**.

**Cross-checked safe against the real MCU firmware side too**
(`hardware/MCU/source/src/uart_protocol.c`, `handle_sync_settings()`,
`case 0x00`, itself traced from the real firmware's own `0x080089F8`
handler earlier this session): value 1 drives GPIOB Pin 1 HIGH, value 0
(or 3) drives it LOW, and value 2 is a distinct, real `AT+UPGRADE`
trigger -- this toggle only ever sends 0 or 1, never touching the
upgrade path.

The now-removed `hal::send_mcu_audio_route()` call (previously piggy-
backed onto this toggle on the theory that `id=0x11` and `CMD 0x84`
drove the same GPIOC13/PC2 relay) was removed along with the wrong id --
that theory's premise (id=0x11 being mic-related) no longer holds, so the
coupling was removed rather than left dangling on a disproven basis.

Real build-verified: `custom_ui` compiles and links clean. Not yet
hardware-tested.

---

## Full trace (2026-08-29): the complete `CMD 0xA0` settings picture, and why most rows have no real name

Continued past the retracted `getSetItemText` labels, chasing the real
per-item name mechanism to its actual source. Real, somewhat surprising
conclusion: **most of the 18 settings genuinely don't have individual
display names in the vendor UI at all.**

### Where the search went

1. `SettingWindow::getSetItemNameList()` / `getSetItemTextList()`
   (`usr/lib/libSetting.so`) -- looked like the obvious place, but both
   turned out to return the **top-level category list**
   (`Common`/`Sound`/`SysInfo`/`AudioSender`/`DateTime`/`Display`/
   `Wallpaper`/`Language`), not per-item names within a category.
2. `SettingWindow::attachItem(QString, QString, QLayout*)` -- the real
   function that adds one row to a category's UI, called from
   `SettingPlugin::customEvent(QEvent*)`. But its two `QString` args are
   read from an incoming `MsnEvent`'s `getParam()`/`getParam2()` at
   runtime, not from static strings in `libSetting.so` -- items are
   registered dynamically by whichever plugin owns them.
3. `MCUAdapter_BoxP300::translateApp()` (`0x38a64`) -- the real sender,
   traced fully. It builds exactly **one** `MsnEvent` with a
   `QVariant(QStringList)` containing **two entries**: the internal key
   `"CarSetting"` and the translated display name `"Car Setting"`
   (via `tr()`), dispatched through `QCoreApplication::notifyInternal()`.
   It also separately calls `TranslateAppTitle(0x203, tr("Car Setting"))`
   to set that app's window title. **That's the entire function** -- it
   registers one category/app, not 18 per-item labels.
4. The `"SetItem%1"` string found earlier is not a UI label either --
   traced its actual use inside `syncSettingDataToMcu(int)` (the
   `QString::arg()` call right after it takes `r4` = `idx` as the format
   argument) -- it's a debug log line (`qDebug() << "SetItem" + idx`),
   not anything user-visible.

### What this means

The `MCUSettingWindow` factory tool (raw `chkA0`-`chkD7` bit checkboxes,
found and ruled out earlier this session) and the "Car Setting" app
registered by `translateApp()` are the two real UI surfaces for this
whole `CMD 0xA0` settings space on the stock hardware -- neither exposes
18 individually-named rows. The two settings this session actually needed
(Camera Type, Microphone) have real distinct names purely because
`getSetItemValueTexts()`'s own **value texts** happen to be self-
descriptive (`"AfterMarket Camera"` IS both the name and the value), not
because the vendor UI shows a separate "Camera Type:" label next to them.

### Final consolidated table -- everything confirmed this session

| id | value texts (confirmed, jump-table-walked) | real distinct name (confirmed) | custom_ui |
|---|---|---|---|
| `0x00` | `OEM Microphone`, `AfterMarket Microphone` | *(self-descriptive values)* | ✅ "OEM Microphone Relay" |
| `0x01` | `AfterMarket Camera`, `Factory Camera`, `AfterMarket 360`, `Factory 360` | *(self-descriptive values)* | ✅ "OEM Factory Camera" |
| `0x02`-`0x07` | *(share id 0x00's handler -- unused/aliased)* | -- | not wired |
| `0x08` | `Off`, `On` | **"Trajectory"** (confirmed, unclobbered check) | not wired |
| `0x09` | `Off`, `On` | -- | ✅ "OEM Factory Microphone" (pre-existing) |
| `0x0A` | `CAN Active`, `12V Active`, `P Key Active` | **"360 camera"** (confirmed, unclobbered check) | not wired |
| `0x0B` | *(empty/dynamic)* | -- | not wired |
| `0x0C` | `Off`, `Radar Active`, `5 s`, `10 s`, `15 s` | **"Front camera"** / **"Front camera time"** (confirmed, unclobbered check, two-part) | not wired |
| `0x0D` | `5 s`, `10 s`, `15 s` | -- | not wired |
| `0x0E` | `Off`, `On` | -- | not wired |
| `0x0F` | `Off`, `On` | -- | not wired |
| `0x10` | `Off`, `On`, `12V Active` | -- | not wired |
| `0x11` | `Off`, `On`, `12V Active` | -- | not wired |

Rows without a "real distinct name" aren't unlabeled by omission -- traced
and confirmed they fall into `getSetItemText`'s two generic fallback
buckets (a real, if unglamorous, finding: the stock UI itself doesn't
distinguish them by name either).

This closes out the `CMD 0xA0` settings-list investigation for this
session -- both features actually needed (Camera Type, Microphone) are
implemented against confirmed real mechanisms; the remaining rows are
documented to the limit of what the real firmware/UI actually exposes.

---

## Cross-check (2026-08-29): Qt translation catalog confirms vocabulary, doesn't resolve remaining ids

User asked whether the real names might be recoverable from the
Chinese/English translation files. Real, useful answer: `msnprofile/lng/`
has 19 `.qm` Qt translation catalogs (`lang_en.qm`, `lang_zh-cn.qm`, etc.).
No `lconvert`/`lrelease` binary was actually installed on this dev
machine (only broken symlinks pointing at a nonexistent
`/usr/lib/qt5/bin/lconvert`) -- wrote a direct Qt `.qm` binary-format
parser instead (documented format: 16-byte magic, tagged sections --
`0x42`=contexts, `0x69`=messages, `0x88`=numerus rules; each message is
`Tag_SourceText`(UTF-8)/`Tag_Context`(UTF-8)/`Tag_Translation`(UTF-16BE,
or the literal marker `0xFFFFFFFF` meaning "no override, use source
text") terminated by `Tag_End`).

**Real, useful result**: `lang_en.qm`'s `[MCUAdapter_BoxP300]` context
lists exactly 12 real strings used by that class: `12V Active`,
`360 camera`, `CAN Active`, `Front camera`, `Front camera time`,
`Left Camera`, `P Key Active`, `Radar`, `Reversing camera`,
`Right Camera`, `Speech button`, `Trajectory`. This **independently
confirms** (from a completely different source than disassembly) the
three names already pinned to specific ids (`Trajectory`=`0x08`,
`360 camera`=`0x0A`, `Front camera`/`Front camera time`=`0x0C`), and
confirms `12V Active`/`CAN Active`/`P Key Active` are `0x0A`'s own value
options, not separate setting names.

**What it does NOT resolve**: `Left Camera`, `Radar`, `Reversing camera`,
`Right Camera`, `Speech button` are confirmed real `BoxP300` strings, but
the `.qm` format carries no per-string numeric id -- only class context.
Re-checked the disassembly specifically for a stack spill/reload of
`idx` that could rehabilitate the later `getSetItemText` checks (`cmp
r8,#15`/`#17`/`#0x87` etc, flagged unreliable in the previous entry
because `r8` gets reused as a scratch/function-pointer register): **none
found** -- no `str r8,[sp,#N]` anywhere in the function's early range,
confirmed by direct disassembly. So these 5 strings' real id mapping
remains genuinely unresolved with manual disassembly tracing; would need
a real decompiler (Ghidra/IDA-quality SSA/register-tracking) to pin down
reliably, not attempted further this session.

Real, honest final state: 5 real ids (`0x0B`,`0x0D`,`0x0E`,`0x0F`,`0x10`,
`0x11` -- 6 slots) remain without a confirmed name, though the likely
candidate pool (5 strings above) is now known even if not individually
attributed.

---

## COMPLETE (2026-08-29): full settings-name resolution via Ghidra, and a real vendor-code inconsistency found

User pointed out Ghidra was available on this machine (`~/tools/ghidra`,
12.1.2, plus a bundled JDK 21) -- used it for real, headless
(`analyzeHeadless` + custom Java scripts under `~/ghidra_scripts/`), and
it resolved what manual disassembly tracing could not: Ghidra's
decompiler correctly tracks `idx` (the real function parameter) through
the whole of `MCUAdapter_BoxP300::getSetItemText(int)` via proper
dataflow analysis -- completely immune to the register-reuse confusion
that made manual tracing unreliable past a certain point (documented in
the "RETRACTION" section above). The decompiled C confirms **13 real,
distinct branches** on the true `idx` value: `0, 7, 8, 9, 10, 11, 12, 13,
14, 15, 16, 17, 0x87` -- each appending exactly one `tr()`-translated
string into a dedicated stack local.

**Method**: decompiled the function (`DecompileAddrs.java`), read off the
`if/else-if` nesting order and which named local (`local_58`...`local_28`)
each branch populates. Cross-referenced against Ghidra's own resolved
string data-references (`DumpFuncStrings.java`, found 10 of 13 directly).
Calibrated the raw `add r5,sp,#N` stack-offset -> Ghidra `local_XX` name
formula (`local_offset_magnitude = 0x60 - N`) against 3 already-
independently-confirmed pairs (id 8/10/12, verified via the pre-clobber
manual trace earlier this session) -- matched exactly, 3-for-3, giving
real confidence in the formula. Applied it to locate the remaining 3
strings' exact literal-pool addresses by hand (`id=7`, `id=0x0E`,
`id=0x11` -- Ghidra's automatic reference scan didn't resolve these 3
specific loads as string data references, for reasons not investigated
further) and decoded them directly from `.rodata`.

### Final, complete `getSetItemText` name table (all 13 real branches)

| id | display name (confirmed) |
|---|---|
| `0x00` | **Reversing camera** |
| `0x07` | **Radar** |
| `0x08` | **Trajectory** |
| `0x09` | **Reversing mode** |
| `0x0A` | **360 camera** |
| `0x0B` | **Front camera** |
| `0x0C` | **Front camera time** |
| `0x0D` | **Speech button** |
| `0x0E` | **DVR** |
| `0x0F` | **Right Camera** |
| `0x10` | **Left Camera** |
| `0x11` | **Microphone** |
| `0x87` | **BT Pin Code** |

(ids `0x01`-`0x06`, `0x09`'s value-list sibling, etc. that aren't in this
list get no distinct name from this function -- the list is padded to a
minimum 2 elements with an empty placeholder string instead.)

### A real, genuine vendor-code inconsistency, not a mistake on this project's part

**`id=0x00`'s real display name is "Reversing camera" -- not
"Microphone"**, despite its `getSetItemValueTexts(0)` value pair being
`"OEM Microphone"`/`"AfterMarket Microphone"` (confirmed separately,
solidly, earlier this session). And **`id=0x11`'s real display name IS
"Microphone"**, despite its own value texts being the unrelated
`"Off"`/`"On"`/`"12V Active"`. These two functions -- `getSetItemText()`
(display name) and `getSetItemValueTexts()` (value options, called by
the real MCU-send function `syncSettingDataToMcu()`) -- are two
independent, hand-written `switch`-equivalents over the *same* `idx`
parameter that have genuinely drifted out of sync in the vendor's own
code. This isn't a mistranscription on this project's side -- both
findings are independently disassembly-confirmed, from two different
functions, and the contradiction is real.

**This does not change what was implemented.** The `custom_ui` mic
toggle (`id=0x00`) and camera toggle (`id=0x01`) were both fixed based on
`getSetItemValueTexts()`/`syncSettingDataToMcu()` -- the pair that
actually determines the wire bytes sent to the real MCU firmware, cross-
checked directly against the MCU's own firmware handler
(`hardware/MCU/source/src/uart_protocol.c`'s `case 0x00`). What the stock
head unit's own "Car Setting" screen happens to *label* that row as
("Reversing camera") is a separate, cosmetic-only question -- interesting
as a real finding about vendor code quality, but not something that
should change the wire-level implementation, which is grounded in the
functions that actually build and send the MCU protocol frame.

### Final, fully consolidated `CMD 0xA0` table (all 18 ids, everything this session found)

| id | value texts | display name | custom_ui |
|---|---|---|---|
| `0x00` | `OEM Microphone` / `AfterMarket Microphone` | Reversing camera *(vendor-code mismatch, see above)* | ✅ "OEM Microphone Relay" |
| `0x01` | `AfterMarket Camera` / `Factory Camera` / `AfterMarket 360` / `Factory 360` | *(none -- self-descriptive values only)* | ✅ "OEM Factory Camera" |
| `0x02`-`0x06` | *(share `0x00`'s value handler)* | *(none)* | not wired |
| `0x07` | *(shares `0x00`'s value handler)* | Radar | not wired |
| `0x08` | `Off` / `On` | Trajectory | not wired |
| `0x09` | `Off` / `On` | Reversing mode | ✅ "OEM Factory Microphone" (pre-existing) |
| `0x0A` | `CAN Active` / `12V Active` / `P Key Active` | 360 camera | not wired |
| `0x0B` | *(empty/dynamic)* | Front camera | not wired |
| `0x0C` | `Off` / `Radar Active` / `5s` / `10s` / `15s` | Front camera time | not wired |
| `0x0D` | `5s` / `10s` / `15s` | Speech button | not wired |
| `0x0E` | `Off` / `On` | DVR | not wired |
| `0x0F` | `Off` / `On` | Right Camera | not wired |
| `0x10` | `Off` / `On` / `12V Active` | Left Camera | not wired |
| `0x11` | `Off` / `On` / `12V Active` | Microphone | not wired |
| `0x87` | *(not part of the 0-17 value table)* | BT Pin Code | not wired |

This closes out the `CMD 0xA0` settings investigation completely -- every
id has now been traced to the fullest extent the real firmware/vendor UI
supports, using both manual disassembly and (once available) real
decompiler-verified dataflow analysis, with the two implemented features
(Camera, Microphone) grounded in the mechanism that actually matters --
the wire protocol -- not the vendor's own (demonstrably inconsistent)
display labels.

---

## CMD 0x90 -- disproven (2026-08-30): checked across 5 real firmware images, appears in none

User asked what `SOC_CMD_DIAG_READ_MEM` (0x90, "Diagnostic Flash/SRAM
readback") actually does, then asked for a thorough check across every
other real `can_app.bin` this project has. Real, decisive result:
**this command never existed in any real DCn32-family firmware. It was
a clean-room fabrication and has been removed from the source.**

### What it claimed to do

The clean-room handler took `[Addr_B3, Addr_B2, Addr_B1, Addr_B0, Length]`,
did a raw pointer dereference at that 32-bit address, and echoed the
bytes back over UART. Had this been real, it would have been a genuine,
working RDP-Level-1 bypass and full-flash-dump primitive -- RDP only
blocks *external debugger* access, not the CPU's own code reading its
own flash, so a UART-exposed arbitrary-read command would sidestep the
whole RDP problem this project spent real effort on elsewhere (see the
SWD/DMA-extraction tooling, `tools/stm32f1-firmware-extractor`,
`tools/stm32f1_dma_dump.py`). That's exactly why it needed a hard check
before being trusted.

### Method: read the real dispatch table directly, in every available firmware

Every real command's own handler in this project's clean-room source
carries a `Real firmware (0x0800XXXX)` disassembly citation -- `0x90`'s
comment had none, the one command in the whole file without one. That
was the first real tell.

Located the real dispatch mechanism directly in this device's own
`hardware/MCU/can_app.bin` (`arm-linux-gnueabihf-objdump -b binary -m arm
-M force-thumb --adjust-vma=0x08004000`, this project's own established
convention): a tight bounded loop --

```
r4 = 0
loop:
  cmd = table[r4].cmd_byte   ; ldrb.w r0,[r0, r4, lsl #3] -- 8-byte stride
  if (cmd == received_cmd) { call table[r4].handler_ptr; break; }
  r4++
  if (r4 < 9) goto loop;      ; cmp r4, #9 -- hard bound, no more than 9
```

-- and the table itself, read directly out of the binary's data
(`(cmd_word, handler_ptr_word)` pairs): `0x81, 0x82, 0xA0, 0xFF, 0xE1,
0x85, 0x84, 0x87, 0x88`. Exactly 9 entries, exactly matching the loop's
own bound -- no room for a hidden 10th command. `0x90` is not among them.

### Checked against every other real firmware this project has

| Firmware | md5 | Real dispatch table |
|---|---|---|
| `hardware/MCU/can_app.bin` (this device) | `bea19bfe...` | `81,82,A0,FF,E1,85,84,87,88` (9) |
| `DCn32-VOLVO-V2.10-20240909` | `bea19bfe...` (byte-identical to this device's own) | same |
| `DCn32-VOLVO-V2.10-20240418` | `61222453...` | same 9 commands (different handler addresses, same set) |
| `DCn32-VOLVO-V3.00-20240403` | `0c636852...` | same 9 commands |
| `DCn32-ACURA-V1.01-20250409` | `e38e5a3c...` | `81,82,A0,FF,E1,85,88` (only 7 -- a genuinely different variant, missing `0x84`/`0x87` too) |

Same real technique used on each (locate the `ldrb.w r0,[r0,r4,lsl#3]`
dispatch-read instruction, resolve its PC-relative table-base literal,
read the table directly from the binary's own bytes). **`0x90` appears
in zero of the 5 real images** -- including a cross-vendor build (Acura)
with a genuinely different command set, which rules out "maybe it's a
Prado-specific thing this device's own dump happens to lack."

### Fix: removed, not just flagged

Real accuracy issue for a project whose whole point is faithful
reconstruction -- left in place, a proven-fictional arbitrary-memory-
read handler is a real liability, not a harmless unused stub. Removed
entirely from `hardware/MCU/source/`:
`SOC_CMD_DIAG_READ_MEM`'s `#define` (`uart_protocol.h`), its dispatch-
table entry, and `handle_diag_read_mem()` itself (`uart_protocol.c`) --
the clean-room table is now 9 entries, exactly matching every real
firmware's own count. Build-verified: `make` in
`hardware/MCU/source/` completes clean, `.text` shrank as expected
(3628 -> 3540 bytes) with the dead handler gone. Confirmed no other tool
in this project (`tools/mcu-probe`, `custom_ui`) ever referenced it.

---

## Broad security sweep (2026-08-30): every real entry point checked for overflows/backdoors, plus a real, previously-undocumented protocol found

Following the `CMD 0x90` fabrication, did a full sweep of every real
interrupt-driven entry point in `hardware/MCU/can_app.bin` for buffer
overflows or other undocumented "backdoor" mechanisms, then fully
decoded a previously-unexamined protocol found along the way.

### Every real entry point, checked directly

| Peripheral | Real vector (confirmed from the actual vector table) | Role | Overflow check |
|---|---|---|---|
| UART2 | `0x08006ECB` | SoC command link (the 9-command dispatch already fully documented) | Clean -- a real 8-slot x 30-byte ring, length hard-bounded `<28` in the RX ISR itself, max write index (26) safely inside each 30-byte slot |
| USART3 | `0x08006ED3` | Real AT-command TX channel -- confirmed via its real peripheral base address `0x40004800` (STM32F105's actual USART3 base) | Clean -- generic ring-drain, no length issues |
| UART4 | `0x08006EB9` | **Not Bluetooth as `docs/1.3.1_MCU_FIRMWARE_DECOMPILATION.md` guessed** -- a real, separate device-identification handshake protocol (decoded below), confirmed via its real peripheral base `0x40004C00` | Clean -- every field-accumulation state has a small hard-coded byte limit (5/9/3/9) checked before write |
| UART5 | `0x08006EC1` | Byte-for-byte the same protocol as UART4 (identical magic values `0x55/0x20/0x32/0x50/0xD3/0xD6`) -- a twin link, not something distinct | Same structure as UART4 -- clean |
| CAN1_RX0 | `0x08007065` | Fixed-size CAN mailbox -> ring-buffer copy, always reads all 8 possible data-byte positions regardless of DLC | Clean -- no CAN-ID filtering of any kind, no magic ID triggers anything special |
| USART1 | `0x08006EC9` | Literally a no-op stub (`bx lr`) | Not a real entry point |

**No buffer overflow found anywhere.** The real firmware's bounds-
checking held up consistently, including in the UART2 RX ISR's unusual
multi-slot design, which looked suspicious on first read (a `*15`
stride/multi-slot indexing scheme) but resolved to a genuinely correct,
safely-bounded implementation once fully traced.

### Real, previously-undocumented finding: UART4/5's real protocol

Traced `0x8007780` (UART4's real handler body) instruction-by-instruction
and found a genuine, small, well-formed protocol, not previously decoded
by this project:

- **Sync byte**: `0x55`. A byte matching this in the idle state transitions
  to a "type" state.
- **Type byte** (the next byte received) selects one of 5 real sub-
  messages: `0x20`, `0x32`, `0x50`, `0xD3`, `0xD6`.
- Types `0x20` and `0x32` are **outbound**: on receiving one, the MCU
  streams a small fixed field back out over the same UART, one byte per
  subsequent interrupt call (5 bytes for `0x20`, 9 bytes for `0x32`).
- Types `0x50`, `0xD3`, `0xD6` are **inbound**: subsequent bytes get
  accumulated into small fixed-size fields (9, 3, 9 bytes respectively).
- All five fields live in one shared struct (base `0x20001365`, confirmed
  identical across UART4 and UART5's own copies of this handler).

**Traced the real populating function for this struct** (`0x80065b4`,
called from... its own callers not individually chased further, out of
scope for this pass) and found it copies from 5 fixed FLASH-resident
source structs (`0x0800BBC8`-`0x0800BBE2`) into the 5 protocol fields.
Reading those flash bytes directly gives a real, concrete result:

- The `0x20`-response field (5 bytes) = `00 00 00 00 FF` -- a zeroed
  placeholder with a `0xFF` sentinel/terminator.
- **The `0x32`-response field (9 bytes) = `"   cD31\0\x93"`** -- a real,
  readable ASCII string (three leading spaces, then `"cD31"`, then a NUL
  and a trailing byte, plausibly a length/checksum). This is very likely
  a real hardware module model/identifier string this MCU announces to
  whatever's connected on UART4/UART5.
- The three inbound fields (`0x50`/`0xD3`/`0xD6`) all default to
  all-zero + a trailing `0xFF` sentinel -- genuine placeholders meant to
  be overwritten by whatever the connected peer sends, not pre-filled
  with real data.

**Real, honest conclusion**: this is a genuine device-identification
handshake protocol with an actual identifier string (`"cD31"`) baked
into flash, connected to *something* via UART4/UART5 that isn't the
Bluetooth module (that's confirmed to be on USART3 instead, via its real
peripheral base address). What specific accessory this is remains
undetermined -- `"cD31"` is a real, concrete clue for further external
research, but chasing down what physical device this identifies is
outside what static analysis alone can settle. No security concern found
here -- properly bounded, benign identification exchange.

### `CMD 0x87`'s PIN-substitution bytes: exhaustively traced, no writer found

Continuing the earlier open question (`CMD 0x87`'s handler embeds 4
bytes read from SRAM `0x20000238+2..+5` into the digit positions of a
static `"AT+PIN=0000\r\n"` template before sending it over USART3):
found and checked **every single reference** to that struct's base
address anywhere in the reachable firmware code (8 total call sites --
`CMD 0xA0`'s id=0x00 handler, `CMD 0x84`, `CMD 0x85`, `CMD 0xFF`, the
main dispatch loop's own checksum-validation routine, and `CMD 0x87`
itself). **None of them write offsets 2-5.** `CMD 0x84` writes offsets
16/17 and reads 94; `CMD 0x85` *reads* offsets 3-5 (copying them
elsewhere, the opposite of what would populate them); `CMD 0xFF` and the
checksum routine only read offset 0-2.

Combined with the earlier finding that this SRAM range falls in `.bss`
(zero-initialized, past `.data`'s real end at `0x200000E8`, confirmed via
this project's own established `.data` flash-source mapping from the
`CMD 0x88` TEA key trace), the honest conclusion is: **these 4 bytes are
very likely always zero for the entire operational life of the
firmware**, meaning the real `"AT+PIN="` frame this firmware transmits
almost certainly contains raw NUL bytes in the digit positions, not a
valid `"0000"` PIN string. This reads as a genuine bug/dead code in the
real vendor firmware (a template-substitution mechanism whose source
data was never wired up) rather than an intentional weak-credential
backdoor -- the command as implemented would send a malformed AT command
that a real BT module's AT parser would most likely just reject, not a
working "PIN reset to a known-weak value" backdoor. Not chased further;
flagged as a real, low-severity firmware-quality finding rather than a
security vulnerability, since a broken/no-op command isn't exploitable.

### Other avenues checked and closed

- **No real vendor MCU bootloader binary exists to check** -- RDP Level 1
  blocks external dumping of it (the whole reason this project has its
  own SWD/DMA-extraction tooling), and this project's own
  `hardware/MCU/bootloader/` is a clean-room reimplementation, not useful
  for backdoor-hunting since it isn't derived from real disassembly.

---

## SRAM code-execution / cold-boot RDP1 bypass -- evaluated, closed by ST's own documented design (2026-08-30)

User proposed a real, legitimate attack class worth taking seriously:
inject a dump payload into SRAM via SWD (unblocked even under RDP1),
configure boot mode to run from SRAM instead of flash, reset, and let
the payload read out flash content via the CPU's own (allowed) access
rather than the debugger's (blocked) access.

**Verified against ST's own documented RDP Level 1 behavior** (not just
this project's own prior findings) via a real web search, cross-checked
against multiple independent sources
([stm32world.com](https://stm32world.com/wiki/STM32_Readout_Protection_(RDP)),
[ST Community](https://community.st.com/t5/stm32-mcus-security/rdp-bootloader-cannot-be-used-together/td-p/237314)):

> Access to protected memories is only allowed when booting from User
> Flash memory, otherwise a system hard fault is generated, blocking all
> code execution until the next power-on reset. When RDP Level 1 is
> active, no access to Flash memory can be performed via debug features
> **even while booting from SRAM or system memory bootloader.**

**This is exactly the proposed attack, and it's closed by design, not
merely untried.** The instant an SRAM-resident payload attempts to read
flash content -- the entire point of the payload -- it hard-faults
immediately, regardless of whether a debugger happens to be attached at
that exact moment. ST built RDP1 specifically to defeat this class of
attack.

**Directly corroborated by this project's own already-confirmed live
hardware behavior**: the BusFault trace in the "CRITICAL SAFETY FINDING"
section above (`PC=0x080004AC` reading flash `0x080004D4`, `CFSR`
decoded as `BFARVALID`+`PRECISERR`) is the *same underlying mechanism* --
RDP1 blocking CPU flash access outside "boot from main flash with no
debugger attached" -- just triggered by a different one of RDP1's two
real trigger conditions (debugger-attached vs. non-flash boot mode; the
proposed attack would hit the latter).

**Real safety note, also verified**: this would NOT have risked
permanently destroying the real firmware if attempted -- mass erase on
STM32F1 only happens when *explicitly downgrading* RDP1 to RDP0 via the
option bytes, not from a boot-mode change while remaining at RDP1. The
real failure mode is the same safe hard-fault-requiring-power-cycle this
project has already hit and knows how to recover from.

**Verdict: closed, not just deferred.** Unlike the `racerxdl`
SWD-protocol-timing-race tool (still the one credible untried avenue,
see "Future reference" above -- it attacks debug-port protocol timing
directly, not CPU execution/fetch, a genuinely different mechanism),
SRAM-code-execution/cold-boot doesn't offer a path this chip's RDP1
doesn't already specifically defend against.

### Related real finding: a stale, misleading claim of success found and retracted

While evaluating this, found `hardware/MCU/live_dumps/README.md` still
made confident, specific-looking claims (real-looking SHA-256 hashes, a
detailed "hardware architecture" analysis) of a **successful** RDP
bypass via `CVE-2020-8004` -- but the `.bin` files it describes don't
exist in this checkout. Git history shows why: they were committed once
(`d737472b`), then deliberately deleted one session later
(`aba08e68`, "remove unreliable live SWD dump files") after being found
to disagree with the verified-correct `can_app.bin` on 99.7%+ of
non-placeholder words, traced to a real bug in the extractor tool
(`tools/stm32f1_extractor_fixed.py`'s broken `address % 0x200` shortcut
reading live CPU register state instead of flash). **The README itself
was never updated or removed alongside the files it describes** -- a
real documentation-hygiene gap that could have misled a future reader
(or a future pass of this same investigation) into believing RDP had
already been bypassed. Retracted in place, original text preserved
for the historical record, real explanation and pointer to this
project's actual findings added.
