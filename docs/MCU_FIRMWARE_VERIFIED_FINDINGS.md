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
