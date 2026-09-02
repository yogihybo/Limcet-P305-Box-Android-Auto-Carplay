# MCU Firmware (`can_app.bin`) — Verified Findings, Correcting Prior Handoff Docs

**Date**: 2026-08-27
**Method**: direct disassembly (`arm-linux-gnueabihf-objdump -b binary -m arm -M force-thumb`)
of the real `hardware/MCU/can_app.bin` (31,996 bytes, SHA-256
`1c21486a2a0bd969de9a9d98c902f1ca572206588d84a9faff377c56614b1c22`),
cross-checked byte-for-byte against the file on disk, not assumed from
prior docs.

## Important scope clarification (added 2026-08-30, applies retroactively to this whole doc)

**`hardware/MCU/can_app.bin` is the generic `DCn32-VOLVO-V2.10-20240909`
firmware, not a dump of the real, Prado-specific firmware actually
flashed on this project's physical unit.** Confirmed by direct
byte-for-byte comparison: `hardware/MCU/can_app.bin` and
`firmware_dumps/MCU/DCn32-VOLVO-V2.10-20240909/can_app.bin` are
byte-identical (same MD5, `bea19bfe...`) -- the same file, not two
independent copies that happen to match. It's a USB update *payload*
this project holds (`hardware/MCU/MCU_FIRMWARE_REVIEW.md` traces its
real provenance and update mechanism in detail), not a live extraction
from the running vehicle. The real firmware genuinely installed on the
physical Prado unit has never been captured by this project -- see
`hardware/MCU/live_dumps/README.md` for the one attempt that claimed to
be exactly that, found fabricated, and retracted.

**Why this doesn't invalidate the findings below.** Every real,
disassembly-confirmed mechanism this doc documents -- UART wire
framing, the 9-entry command dispatch table and its handler logic,
`CMD 0x88`'s TEA cipher, `CMD 0xE1`'s reboot-to-bootloader trick, RDP1
behavior, the ring-buffer bugs, GPIO pin functions -- is architecture/
firmware-codebase-level, not vehicle-profile-level. This project's own
5-firmware cross-check (the "CMD 0x90 -- disproven" section, and the
shared-TEA-key section) already established that all 5 real reference
images it holds -- this Volvo build included -- are the same `DCn32`
codebase with only CAN IDs and vehicle-specific tables differing.
**What is NOT trustworthy as Prado-specific**: the CAN ID tables
(Mode 1/2/3 dispatch, §4 of `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`)
and any vehicle-profile-specific value -- those are real for a Volvo,
not confirmed for a Toyota Prado. Several places in this doc refer to
"this device's own `can_app.bin`" as informal shorthand for "the
`can_app.bin` file this project holds as ground truth" (as opposed to
one of the other 4 reference images checked for cross-comparison) --
read that phrase with this clarification in mind, not as a claim that
this exact binary is confirmed to be what's currently flashed on the
physical vehicle's MCU.

---

## Why this doc exists

`docs/HANDOFF_MCU_AUDIO_I2C.md` and `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`
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

## Cross-check against `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`: tabular data is genuinely reliable

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

## Real, user-observed finding (2026-09-01): SWD-attached battery drain

Two real, empirical observations from the user, on the actual vehicle, not
inferred:

1. The MCU stays powered continuously even with the car off (an SWD
   indicator LED on the debug probe stayed lit) -- confirms the STM32
   sits on a permanent/`VBAT`-style rail, not switched by ignition/ACC.
   Consistent with the "arm-then-trigger" `CMD 0xA0 id=0x11` design
   (see that section above) needing to poll `GPIOB Pin 2` continuously
   regardless of ignition state.
2. **Leaving an SWD debugger connected for an extended period measurably
   drained the vehicle battery; leaving it disconnected did not** (charge
   was retained normally). These are genuinely different power states,
   not the same baseline drain observed twice.

**Likely mechanism** (a real, well-known Cortex-M behavior, not yet
independently re-verified against this specific chip's `DBGMCU`
configuration): most Cortex-M debug setups disable the core's normal
sleep/`WFI` entry for as long as a debugger session is attached (the
`DBGMCU_CR` register's `DBG_SLEEP`/`DBG_STOP`/`DBG_STANDBY` bits exist
specifically to control this) — unless explicitly configured otherwise,
attaching a debugger very plausibly forces the core into sustained
full-active current the whole time it's connected, on top of whatever the
probe itself draws. The always-on `VBAT`-rail finding above establishes
the *idle* baseline is apparently fine on its own; it's specifically the
*debug-attached* state that changes the power profile.

**Practical implication for any future SWD work on this platform** (this
session's own picopwner/CAN-sniffing planning docs, any future
`flag_5e`/reverse-gear live-state verification, etc.): treat an
attached debugger as an active battery drain, not a passive one. A
quick attach-check-detach session is fine; leaving the probe connected
for an extended window (overnight, a multi-day test, etc.) risks
draining the vehicle battery unless the engine is running or the
battery is on a charger. Not yet quantified (no measured current draw
figures) -- this is a real, reported qualitative effect, not a
precise number.

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

- `docs/HANDOFF_MCU_AUDIO_I2C.md` / `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s
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
multiplexer per `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s claim for
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

## Found the real GPIOB Pin 2 autonomous-relay scanner, and traced its 4 sibling pins (2026-09-02)

Follow-up to the `CMD 0x82`/`CMD 0x84` gate-sharing trace (`docs/MCU_COMMAND_REFERENCE.md`):
while locating the shared relay dispatcher's (`0x080058A4`) real callers, found a genuinely
new, free-standing function -- `0x080084A4` -- reached from **neither** SoC->MCU dispatch
table nor the `CMD 0xA0` id-switch. It's a periodic GPIO scanner:

- Reads 5 real pins via a shared `port_base + mask` helper (`0x08005582`), each resolved from
  its own wrapper's literal-pool port-base value (STM32F105 standard bases): **`GPIOA Pin 8`**,
  **`GPIOC Pin 9`**, **`GPIOC Pin 8`**, **`GPIOC Pin 7`**, **`GPIOB Pin 2`** -- packed into
  bits `0`-`4` of one combined byte.
- Change-triggered: compares against the previous scan (`0x080085d8`), returns immediately if
  unchanged -- not a naive act-every-poll loop.
- `GPIOB Pin 2` (bit `4`), debounced and inverted, writes the shared `0x5e` flag
  (`0x20000236`) and calls the relay dispatcher directly (`0x0800859e`/`0x080085bc`/
  `0x080085ce`) -- **this is the literal source of the already-hardware-confirmed autonomous
  OEM-camera-relay switching**, not just corroborating evidence for it.

**Traced the other 4 pins' consumers this pass** (flagged as a follow-up in the command-ref
doc, chased down now):

- `PA8`/`PC9` (bits `0`/`1`) combine into a debounced 2-bit value at struct offset `0x36`
  (`0x2000020E`). Real consumer found: a dispatch function around `0x08007f68` reads this
  value and, depending on whether it's `1`/`2`/`3`, picks between **3 separate, differently-
  sized const lookup tables** (`0x0800bae8`/`0x0800bb30`/`0x0800bb80`, 9/10/? entries) to match
  an incoming key value (read from a different struct, offset `0x12d`) against a handler
  pointer -- structurally identical to a mode-dependent CAN-message-ID dispatch table swap.
- `PC8`/`PC7` (bits `2`/`3`) combine into a debounced value at offset `0x46` (`0x2000021E`).

**CORRECTION (2026-09-02, same day): the `0x0800910c` function is not an inert diagnostic
readback -- it's a real, load-bearing `CMD 0x30` (Arkdata display-profile selector) sender,
and `PC8`/`PC7` specifically feed its one meaningful case.** Re-derived `0x08007e0c`'s exact
calling convention mechanically this time (see the `CMD 0x82` section of
`MCU_COMMAND_REFERENCE.md` for the full derivation: second argument = `len`, third = `cmd`),
correcting the earlier "reported back over the bus" framing, which never identified which real
command this actually was. `0x0800910c` takes a selector argument (`r4`, presumably the
scheduler item id -- see below) and branches:

- `r4==0`: sends `CMD 0x30` payload `[0x00, <PA8/PC9's 0x36 value>]`.
- `r4==12`: sends `CMD 0x30` payload `[0x0C, <PC8/PC7's 0x46 value>]` -- **`payload[0]==0x0C`
  is the exact, single sub-type `MCUAdapter_BoxP300`'s real SoC-side dispatch treats as
  meaningful** (already documented elsewhere in this doc set as the real trigger for rewriting
  `/msnprofile/arkdata.ini`), so this is a genuine, real consequence, not a no-op.
- any other `r4`: sends `CMD 0x30` payload `[0x00, 0x00]` -- a real send, but a confirmed no-op
  on the receiving end per the same SoC-side dispatch rule.

**So `PC8`/`PC7`'s combined value is very likely a real selector for which arkdata display
profile gets loaded**, not just a readback value -- `PA8`/`PC9`'s value, by contrast, only
ever reaches the SoC through the confirmed-inert `r4==0` sub-type, so its role stays limited
to the internal CAN-dispatch-table selection already found above. Exact mapping from
`PC8`/`PC7`'s raw 2-bit value to a specific arkdata profile name wasn't traced further --
the SoC-side `arkdata.ini` rewrite logic (`libMcuCenter.so`) would need its own pass.

**`r4`'s real source, traced as far as time allowed**: `0x0800910c` is called with a fixed
type argument via the same priority-based event scheduler documented in the `CMD 0x82`
section above (`0x0800B8A0` → `0x08005B90`, type-indexed dispatch table). Item id `0` and item
id `12` (matching "bit `6`" in that scheduler's priority list) both plausibly route here --
not confirmed to the exact table slot this pass, same honest gap as `CMD 0x82`'s own Site 1
trigger.

**Real correction to the follow-up note left in the command-ref doc, still standing**: these 4
pins are still NOT good candidates for `CMD 0x06`'s (or any other command's) still-unconfirmed
vehicle-dynamics bit meanings -- their real, traced behavior (mode-selecting between internal
CAN dispatch tables; a real but narrow arkdata-profile trigger) still reads like a **hardware
board-variant/configuration selector** (matching this firmware's own multi-vehicle-brand build
convention, e.g. the `DCn32-VOLVO`/`DCn32-ACURA` naming already documented at the top of this
doc) rather than a live door/handbrake/turn-signal sensor input -- that part of the original
finding holds even after this correction to how "diagnostic readback" was characterized.

**Scope note**: none of this is part of the SoC<->MCU UART protocol `MCU_COMMAND_REFERENCE.md`
catalogs -- it's internal MCU-side CAN-bus/board-configuration machinery, which is why it
lives in this "deep trace" doc rather than the command reference. Not chased further: the
exact 9/10-entry table contents (which CAN message IDs each mode's table actually handles),
and what physically drives `PA8`/`PC9`/`PC8`/`PC7` on this board (DIP switches, resistor
straps, or something else) -- real, bounded follow-ups if this ever matters again.

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
| 15 | "Right Camera" | `0x0f` | **CORRECTED (2026-08-31)** — earlier "confirmed no GPIO effect" was wrong: struct offset `0x43` IS read (`0x08005D30`), gated by a second flag, and drives the same GPIOA Pin 15 / GPIOB Pin 8 / GPIOB Pin 9 relay trio as `id=0x0b`'s PA15/PB8/PB9 subsystem. Confirmed via GPIO port-base literal resolution (`0x40010800`=GPIOA, `0x40010C00`=GPIOB) and byte-identical in the `DCn32-ACURA` firmware dump too |
| 16 | "Left Camera" | `0x10` | **CORRECTED (2026-08-31)**, same finding as idx 15: struct offset `0x44` is read (`0x08005D80`, adjacent code, different gating flag), driving the identical PA15/PB8/PB9 trio |
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
| UART4 | `0x08006EB9` | **Not Bluetooth as `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md` guessed** -- a real, separate device-identification handshake protocol (decoded below), confirmed via its real peripheral base `0x40004C00` | Clean -- every field-accumulation state has a small hard-coded byte limit (5/9/3/9) checked before write |
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
  accumulated into small fixed-size fields. **Correction (2026-08-30,
  re-traced precisely against each continuation handler's own entry
  point and length check, not from memory)**: the real sizes are
  `0x50`=3 bytes (struct offset `+0x4e`, continuation entry `0x8007908`),
  `0xD3`=9 bytes (offset `+0x3f`, entry `0x80078ea`), `0xD6`=9 bytes
  (offset `+0x5d`, entry `0x8007926`) -- this doc previously listed
  these in a different order ("9, 3, 9 respectively" for
  `0x50`/`0xD3`/`0xD6`); `0x50` and `0xD3`'s sizes were swapped.
- All five fields live in one shared struct (base `0x20001365`, confirmed
  identical across UART4 and UART5's own copies of this handler).

**Real, previously-uncaptured finding (2026-08-30): the `0x20`-type
outbound field is NOT static -- something outside this ISR actively
refreshes it, and that something also posts real SWC-style key
events.** Searching every reference to the struct base `0x20001365`
across the whole binary (not just inside the two ISRs) found a 4th,
genuinely external hit at `0x800b990`, in a function starting around
`0x800b8a0` -- far from either UART ISR. That function:
- Decodes individual bits of a status byte at struct offset `+5`
  (masks `0x10`/`0x02`/`0x04`/`0x20`/`0x40`/`0x80`/`0x01`) and, for two
  of them, calls the exact same internal event-post primitive
  (`0x80062fc`) with the exact same literal values (`0x4101`/`0x4001`)
  this project already found and documented elsewhere as a real
  Toyota-profile SWC key-press/release pair (see the earlier "one
  handler fully decoded" CAN-dispatch finding, `mode=1 CAN ID 0x105`'s
  handler) -- strong evidence this function is on the same key-event
  path, not a coincidental reuse of those constants.
- For most other bit patterns, instead sets a local "mode" value
  (`r5` -- observed values `0`, `1`, `2`, `3`, `4`, `12`, `13`, `36`)
  and falls through to a shared tail (`0x800b94c`) that -- gated on a
  couple of flag bytes and `r5` not being `3`/`4` -- **copies struct
  offset `+4` (a 4-byte word) and `+8` (a byte) directly into struct
  offset `+0x21`**, i.e. straight into the `0x20`-type field's own 5
  bytes, overwriting whatever the flash-populate function
  (`0x80065b4`) set there at power-on (`00 00 00 00 FF`).

**What this means, stated at the right confidence level**: the `0x20`
query's real response is only `00 00 00 00 FF` *before* this update
function has ever run. Once it runs (on whatever real trigger calls
it -- not yet traced back further this pass), the response reflects
whatever is currently at struct `+4`/`+8`, which this same function's
neighboring code treats as related to vehicle status/alert bits, not a
fixed hardware identifier. This ties the UART4/5 link -- previously
characterized as a self-contained "device identification handshake" --
into the same internal event/status pipeline the CAN-bus key-decoding
logic uses elsewhere in this firmware. **Not yet resolved**: what
actually calls this function (a periodic poll? a specific CAN frame's
handler? gated on real vehicle state?), and what struct `+4`/`+5`/`+8`
concretely represent beyond "status/alert-adjacent." A real, bounded
follow-up, not chased further this pass.

**Focused re-check (2026-08-30), specifically for the classic
"unbounded UART-fed buffer write -> stack/adjacent-memory overflow"
vulnerability class** -- re-verified against the now-corrected field
sizes above, not assuming the general "no overflow found anywhere"
conclusion from the earlier broad sweep still holds without checking.
Traced the exact write-vs-check instruction order for all three
inbound types (`0x50`, `0xD3`, `0xD6`), the ones that write
attacker-controlled bytes: each continuation handler writes the
received byte at `field[idx]` *before* checking `idx` against the
field's size (`0x8007850`/`0x78ea`/`0x7908`/`0x7926` region) -- the
same "check-after-write" shape that would be genuinely dangerous if
`idx` could ever exceed the field bound first. It can't, verified two
ways: (1) `idx` is explicitly seeded to `0` at session entry for all
three types (`0x786a`/`0x785a`/`0x787a`), and (2) the bounds check
fires immediately after the write that brings `idx` to the field's own
size (`3`/`9`/`9`), at which point it resets *both* the outer state
variable and `idx` to `0` (idle) before any further byte can be
processed -- so the write index used on any given call is always
strictly `< field_size`, never equal to or past it. A byte received
mid-session that happens to match another type byte (e.g. `0x55` or
`0x20`) is still just treated as ordinary accumulated data, not
re-interpreted -- no state-confusion path either. **No overflow, out-
of-bounds write, or out-of-bounds leak exists in this protocol's real
implementation**, for either direction (inbound writes or outbound
reads), under this specific attack framing. This firmware also has no
shared, generic "CheckUART"-style length-validation routine the way
the vulnerability class describes (each protocol -- `USART2`'s `0x2E`
frames, this `0x55`-sync protocol -- implements its own independent,
hardcoded-size state machine) -- there's no single validation function
whose failure would compromise multiple protocols at once.

**Real confirmation, added 2026-08-30 in response to "does the firmware
actually use UART4/5" -- yes, genuinely, not just a populated vector
table entry.** Traced the real init code for both peripherals (call
sites at `0x8007718`/`0x80079de`, right before each handler in the
binary): both explicitly enable their `RCC->APB1ENR` clock-gate bit via
a shared helper function (`0x8006860`, a generic
`rcc_apb1_clock_gate(mask, enable)`) --
`lsls r0, r1, #19` (`UART4EN`, bit 19) for UART4, `lsls r0, r1, #20`
(`UART5EN`, bit 20) for UART5, both with `enable=1`. Each call site is
also preceded by two calls to a GPIO pin-config helper (TX+RX pin
setup) and followed by a real `USART_Init()`-style config call with a
populated struct -- **both peripherals are configured for 9600 baud**
(`mov.w r0, #9600` written into the struct's baudrate field, verified
via the actual init-function call, not assumed). This is new,
previously-uncaptured information -- the original protocol decode
above documented the wire format but not the baud rate. If ever
testing this protocol against a real candidate port, try 9600 baud
first as the disassembly-confirmed value, not just a guess from the
existing 9600/19200/38400/115200 fallback list this project uses
elsewhere.

**Real exchange mechanic, traced from the ISR's own state handlers
(2026-08-30) -- not a burst reply.** Checked what actually drives each
byte of the two outbound fields (`0x20`/`0x32`) out over the wire.
Field byte 0 is sent immediately, inline with processing the type byte
itself (`0x781e`-`0x7834` for type `0x20`, `0x783a`-`0x7852` for type
`0x32`). But every *later* byte of the field is only sent from the
`state==0x20`/`state==0x32` handlers (`0x800788c`, `0x80078ba`), which
only run again when the next `RXNE` interrupt fires -- i.e. only after
the querier sends one more byte (any value; it's never read back, only
its arrival matters, since the byte-out index at struct offset `+123`
is what actually advances). This is a clocked, one-in/one-out exchange,
not "send a query, get the whole field back." `tools/uart45-probe/`
was updated to actually pump the remaining bytes out this way -- its
first version only sent the 2-byte query and listened once, which
would have silently captured just `field[0]` and gone quiet, an
incomplete/misleading test of its own hypothesis. Fixed before this
was ever run on real hardware.

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

**Real candidate for the physical destination, found 2026-08-30 while
investigating whether any other tty on the Linux side had been missed.**
`docs/1.9_KERNEL_REFERENCE.md`'s own DTS-derived hardware map labels the
SoC's `&uart3` (`/dev/ttyS2`) as **"STM32 companion MCU"** -- a
physically separate UART link from `ttyHS0` (which uses the SoC's
`ark-hsuart` peripheral, a different controller family entirely, not
the generic `uart0-3` block `ttyS2` belongs to). `/dev/ttyS2` ("MSNEry")
is independently confirmed live (real traffic observed) but its peer
was never identified (`docs/historical/1.3_MCU_ADAPTERS.md`, `tools/uart-test/`) --
exactly the profile of an orphaned link that would match this
UART4/UART5 protocol, since USART2 (SoC command link) and USART3
(Bluetooth) are both already accounted for elsewhere, leaving UART4/
UART5 as the STM32's only unaccounted-for peripherals.

**Not yet confirmed -- this is a real, well-grounded hypothesis, not a
finding.** `tools/uart45-probe/` (added this session) sends the real
`[0x55, 0x20]`/`[0x55, 0x32]` identify queries this protocol actually
uses and listens for the expected response (`00 00 00 00 FF` / a real
`"cD31"` string) against `/dev/ttyS2` by default. A positive result
would close this open question outright; a negative one at every baud
candidate would be equally useful (rules the hypothesis out cleanly).
Blocked on hardware access to run, same as the other live-device
experiments recorded in this doc.

**Real cross-check against the actual `strace`-captured traffic
(2026-08-30), weakens this hypothesis -- recorded honestly, not
dropped.** The real 2026-07-22 `strace` capture of `ttyS2`
(`docs/logs/directfb_strace.txt`) shows `MsnCoreApp` writing
`[0xFA][arg1][arg2][arg3][len][payload...][chk][0xAF]`-framed data to
this port -- a completely different frame format from the `[0x55]`-sync
UART4/UART5 protocol this hypothesis is built on. Checked `can_app.bin`
directly for any trace of `0xFA`/`0xAF` framing anywhere in the image
(every one of the 144 `0xFA` bytes in the file, not a sample): all of
them resolve to either a coincidental `cmp r0/r4, #0xfa` (`#250`)
ring-buffer wrap-bound check -- unrelated to framing, sitting right
next to the confirmed real `0x2E` sync-byte check -- or two unrelated
bytes inside separate multi-byte Thumb-2 instruction encodings. **No
`[0xFA]...[0xAF]` protocol implementation exists anywhere in this
firmware image.**

If `ttyS2` really is wired to a UART this same firmware implements,
this firmware should recognize the framing it's actually being sent --
it doesn't. Two honest readings, can't distinguish between them without
a real live test: (a) `ttyS2`'s peer is a genuinely different board/
chip, not this MCU at all -- matching `docs/historical/1.3_MCU_ADAPTERS.md`'s own
original speculation (a separate steering-wheel-control bridge, amp/
DSP controller, etc.); or (b) the real, uncaptured Prado-specific
firmware (this project only has the generic `DCn32-VOLVO` reference,
see this doc's own scope-clarification section) implements an
additional protocol the Volvo build simply doesn't have. `uart45-probe`
is still worth running -- it's cheap and the `[0x55]` protocol might
still be reachable via `ttyS2` at some baud even if it's not what
produced this specific capture -- but this cross-check means a silent
result there shouldn't be read as ruling out "ttyS2 = this MCU"
entirely; it only rules out this one specific sub-hypothesis.

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

---

## Real firmware bug found (2026-08-30): CAN1 RX ring buffer has no "full" detection -- producer can silently overwrite unread frames

Continued digging per explicit request ("there must be some bugs in
there"). Found a real, concrete bug in `can_app.bin`'s own CAN1 receive
path, not present in this project's own clean-room `can_driver.c` (which
correctly implements the standard "reserve one slot" full-detection
pattern this real firmware is missing).

### The real structure, traced precisely

- **Producer**: `CAN1_RX0` ISR (`0x08007065`, confirmed via the real
  vector table). Reads a shared struct's head index (offset `0x12c`),
  copies the CAN mailbox into `ring[head]` (a **15**-slot, 20-bytes/slot
  array at struct base `0x200002bc`), then **unconditionally**
  `head = (head + 1) % 15` -- no check of any kind against the consumer's
  read position before overwriting a slot. (Corrected 2026-08-30 from an
  earlier, wrong "16-slot"/`% 16` claim in this same section -- the real
  wrap check, read directly from the ISR's own disassembly, is
  `cmp r0,#15; blt <skip-reset>`, i.e. head cycles through indices
  `0..14` and resets to `0` on reaching `15`: a genuine 15-slot ring.
  This matches `hardware/MCU/MCU_FIRMWARE_REVIEW.md` and
  `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s independent "15-slot"
  finding -- this doc's own number was the one that was wrong, found via
  a cross-doc consistency check.)
- **Consumer**: a loop inside the vehicle-CAN dispatch function (starts
  around `0x8007fa0`, drains via `0x8007ff2` onward). This side IS
  correctly guarded: after popping and dispatching one frame (tail index
  at offset `0x12d`, same struct), it checks `head == tail` (`0x8008034`-
  `0x8008042`) and only continues looping while there's more to drain --
  a real, correct emptiness check. (An earlier pass of this same trace
  briefly mis-read this as unguarded, having looked at an out-of-context
  fall-through slice of the function in isolation -- corrected once the
  real loop structure, including this check, was found.)

### The real bug

**The producer side has no reciprocal "is the ring full" check.** A
properly-designed lock-free single-producer/single-consumer ring buffer
needs *both* sides guarded -- the consumer must not read past an empty
buffer (real firmware does this correctly) *and* the producer must not
write past a full one (real firmware does **not** do this at all). This
project's own clean-room `can_driver.c` gets this right (`next_head !=
tail` check before writing, deliberately sacrificing one slot to make
full unambiguous from empty) -- the real firmware never does.

**Real, concrete trigger condition**: if 16 or more real CAN frames
arrive on the vehicle bus before the main loop's dispatch function gets
a chance to run even once (plausible during a burst of simultaneous ECU
broadcasts, or if the MCU is busy servicing a UART command for a
stretch), the 17th incoming frame's ISR write silently overwrites
`ring[0]` -- data no one has read yet. Worse than simple loss: if the
consumer happens to be mid-`memcpy` out of that exact slot when the ISR
preempts it (the ISR can fire at any point, including mid-consumer-loop,
since this is a real interrupt vs. main-loop relationship, not
mutually exclusive), the consumer could process a **torn, partially-
overwritten frame** -- acting on genuinely corrupted CAN data (wrong ID,
wrong bytes) rather than either the old or new frame cleanly.

**Real-world impact**: this is the same ring that feeds the Mode 1/2/3
vehicle CAN dispatch tables (steering wheel controls, reverse gear,
etc.) already documented elsewhere in this file -- a burst-triggered
version of this bug would manifest as occasional dropped or corrupted
button presses / gear-state transitions under heavy CAN bus load, not a
security vulnerability but a genuine real-world reliability bug in the
stock vendor firmware.

Not fixed in this project's own clean-room `can_driver.c` since that
file already implements the correct pattern independently -- recorded
here purely as a real, disassembly-confirmed fact about the actual
vendor firmware's own behavior, for anyone testing against real hardware
who observes an occasional dropped/garbled CAN-sourced input under load.

---

## Real firmware bug found (2026-08-30): the SAME missing "full" check exists on the SoC-facing UART2 link too

Continued digging specifically on the SoC-facing side per explicit
request. Found the identical bug pattern as the CAN1 ring (previous
section), on the link `custom_ui`/`tools/mcu-probe`/every other
SoC-side tool in this project actually talks to the MCU over.

### Real structure, precisely re-traced (an earlier read of this exact
### ISR during the same pass briefly suspected a length-accounting bug
### -- `struct[2]` becomes `length + 3` rather than staying as the raw
### length -- but re-verifying against the real starting index (`struct[3]`
### starts at 2, not 0, correctly skipping the already-written cmd/length
### bytes) shows this is exactly correct: `(length+3) - 2 = length+1`
### trailing bytes get written, matching the real frame format
### `[CMD][LEN][PAYLOAD(len bytes)][CHECKSUM(1 byte)]` exactly. Recorded
### here so this false alarm doesn't get rediscovered.)

The real UART2 RX ISR (`0x08006ECB` -> `0x08007298`) implements an
8-slot, 30-bytes/slot ring (write index at struct offset `0`, matching
the same struct this session's earlier `CMD 0xA0`/`0x84`/etc. findings
already use). The **consumer** side (`0x0800720C`, called once per main
loop iteration) correctly gates on a real "is empty" check
(`0x080086A0`: `return (struct[0] == struct[1])`) before dequeuing --
same correct pattern as the CAN1 ring's consumer.

**The producer (the ISR itself) has no reciprocal check.** Its slot-
advance code (`0x0800738A`-`0x0800739A`) does exactly:
```
struct[0]++
if (struct[0] >= 8) struct[0] = 0
```
-- no comparison against `struct[1]` (the consumer's read index) at all,
identical in structure to the CAN1 producer bug.

### Real, concrete trigger condition

If 8 or more complete `[0x2E]...`-framed SoC->MCU commands arrive over
`/dev/ttyHS0` before the main loop's dequeue function runs even once,
the 9th command's ISR-side completion silently overwrites slot 0 --
data no one has processed yet. As with the CAN1 case, if the main loop
is mid-copy out of that exact slot when the ISR preempts it (a real
possibility, not just a race in theory -- the ISR fires asynchronously
relative to the main loop), the consumer could dequeue a torn,
part-old/part-new command rather than either cleanly.

**Real-world relevance for this project specifically**: this is the
exact link `custom_ui`'s `hal::sync_setting()`/`send_mcu_frame()` calls
use, and any future code that sends several `CMD 0xA0` settings-sync
frames back-to-back in a tight loop (e.g. a "restore all settings on
boot" sweep) without pacing them could plausibly hit this -- 8 frames is
not a large burst for software to produce faster than a 38400-baud UART
can drain them (a ~13-byte frame takes ~3.4ms to transmit at that baud
rate; 8 of them back-to-back is ~27ms, well within reach of a tight send
loop with no delay). Worth keeping frame sends paced/acknowledged rather
than fired in a rapid unthrottled burst, given the real firmware has no
protection against this on its own.

This project's own clean-room `uart_protocol.c` does not reproduce this
bug -- its own RX frame ring (`g_rx_ring`/`g_rx_head`/`g_rx_tail`) already
implements the correct `next_head != g_rx_tail` guard before advancing,
same as `can_driver.c`. Recorded here purely as a real, disassembly-
confirmed fact about the actual vendor firmware's own behavior, and as
practical guidance for how to drive it safely from the Linux side.

---

## Real finding (2026-08-30): `CMD 0x88`'s TEA challenge is a full decryption oracle -- the anti-clone scheme was breakable over UART alone, no RDP bypass ever needed

Continued the "what else can we trigger over UART" question. Re-examined
`CMD 0x88` (already disassembly-confirmed earlier this session: real
TEA algorithm, real recovered key) specifically for what it reveals as
a *live, remotely-triggerable* primitive, not just a static fact about
the flash image.

**Real behavior, confirmed via this project's own clean-room
`handle_crypto_challenge()` (built directly from the real disassembly)**:
send any 8-byte value over `CMD 0x88`, the MCU decrypts it with the real,
fixed key and echoes the full 8-byte plaintext straight back. No rate
limiting, no attempt counter, no gating on any other state -- a pure,
stateless decryption oracle.

**Real, standalone security conclusion, independent of this project's
own key-recovery-via-flash-analysis work**: combined with the already-
documented fact that the key has only ~32 bits of effective entropy
(each of its 4 words has just its low byte set), this oracle alone --
reachable over the same `/dev/ttyHS0` link every other command uses, with
**zero need for RDP bypass or flash access of any kind** -- would have
been sufficient to fully break this "anti-clone" authentication scheme.
A 32-bit keyspace is trivially brute-forceable via chosen-ciphertext
queries against a live oracle (send a crafted ciphertext, observe the
plaintext, compare against expected structure to narrow the key). The
real, practical implication: **the entire RDP-bypass effort this project
spent real time on was never actually necessary to defeat this specific
mechanism** -- the vulnerability was reachable the whole time, over the
wire, from the Linux SoC side alone.

### Also checked and ruled out as a leak: `CMD 0x85`

`CMD 0x85`'s handler reads struct `0x20000238` offsets 3-5 and forwards
them via `bl 0x8006228`/`0x80062fc` -- traced these two functions fully
and found them to be a generic **internal event/message-queue primitive**
(a 40-slot circular queue, `r0`=event type, `r1`/`r2`=data, real
full/head/tail bit-flag bookkeeping) used by multiple commands
(`CMD 0x81`'s init handshake posts 4 such events at startup too). Nothing
here reaches a UART TX path -- it's purely internal firmware-to-firmware
messaging, not an external echo. Not a leak.

Other real UART outputs (`CMD 0x7F` version string, `CMD 0x30`
telemetry, the knob/touch/reverse-gear event frames) are intentional,
designed-to-be-read status queries -- nothing secret is exposed by them,
unlike `CMD 0x88`.

---

## Real, still-open RDP1 bypass avenue found (2026-08-30): FPB + power-glitch (`stm32f1-picopwner`)

User shared a Medium article on STM32 shellcode-via-UART firmware
dumping. Checked it against everything found this session:

**The article's own technique doesn't apply here.** It requires a
pre-existing buffer overflow in the target's own UART command parser to
inject shellcode -- this session already checked this firmware's UART
surface exhaustively (twice, independently) and found no exploitable
overflow anywhere (see the "Application-level parsing" sweep above).
No foothold for that specific approach on this firmware.

**But the same search surfaced something real and not yet closed**:
[`CTXz/stm32f1-picopwner`](https://github.com/CTXz/stm32f1-picopwner) --
a real, published, STM32F1-specific RDP Level 1 bypass (Obermaier/
Schink/Moczek's FPB + glitch attack), genuinely different from both
`CVE-2020-8004` (ruled out earlier -- requires the debugger to stay
attached to observe leaked state, which triggers RDP1's own block) and
the SRAM-cold-boot idea evaluated just before this (which changes BOOT
pin configuration, exactly the condition RDP1's documented design
checks for).

### Why this one is genuinely different, not closed by anything found so far

1. **While the debugger is still attached**, configure the Cortex-M3's
   **FPB (Flash Patch and Breakpoint) unit** -- a real debug comparator
   that redirects instruction *fetches* at a chosen address elsewhere.
   Point it at `0x00000004` (the reset-vector-pointer fetch) to redirect
   into SRAM.
2. Load exploit code into SRAM, **then disconnect the debugger**. FPB
   patches genuinely **persist across reset** -- a real, documented
   Cortex-M3 property.
3. **Power-glitch** the target: cut power briefly and restore it exactly
   as `NRST` drops low, timed so SRAM retains its contents through the
   brief loss.
4. **Critically, `BOOT0`/`BOOT1` stay configured for completely normal
   "boot from main flash"** -- the same mode used in ordinary operation.
   This is the real distinction from the SRAM-cold-boot idea already
   ruled out: RDP1's documented block specifically triggers on "boot
   mode != main flash" or "debugger attached." This technique satisfies
   neither condition -- to RDP1's own boot-mode check, this looks like a
   completely ordinary boot. The FPB redirect is a debug feature, not
   part of that gate at all, so the reset-vector fetch gets silently
   hijacked to SRAM without RDP1 having a documented reason to object.

### Real requirements -- shares hardware with the already-documented `racerxdl` avenue

- Raspberry Pi Pico (attack board) -- same requirement already on record
  for the `racerxdl` SWD-timing-race tool above.
- A controllable **`NRST`** line -- this project's current SWD wiring has
  none (`SWDIO`/`SWCLK`/`GND`/`VCC` only), same gap already flagged for
  `racerxdl`.
- Controllable, program-timed **power switching** to the target -- same
  requirement already flagged for `racerxdl`.
- **New requirement, not previously flagged**: `BOOT1` pulled high via a
  1k-100k resistor to 3.3V (per the tool's own documented safety
  requirement). Whether this pin is accessible on this board is
  unverified.
- Real, honest limitation from the tool's own docs: no published success-
  rate data, and the tool's own notes mention the exploit "may be patched
  in 2020+ chip revisions" -- this board's exact silicon revision/date
  code is unverified.

### Real wiring table and USART choice, re-checked directly against the repo (2026-08-30)

Pulled the actual current README rather than relying on the earlier
search summary. Real Pi Pico <-> STM32F1 wiring:

| Pi Pico | STM32F1 |
|---|---|
| GND | GND |
| GPIO1 (UART0 RX) | `USARTx_TX` (dump output) |
| GPIO2 | `VDD` (power-glitch control) |
| GPIO4 | `NRST` |
| GPIO5 | `BOOT0` |

Real USART TX pin mapping the tool supports: USART1=`PA9`,
USART2=`PA2`, USART3=`PB10`.

**Real, direct cross-check against this project's own confirmed pin
findings -- USART1/`PA9` is the clean choice, not an arbitrary pick.**
This session already confirmed USART2's real TX pin is `PA2` (the SoC
link, `ttyHS0`) and USART3's real TX/RX pins are `PB10`/`PB11`
(the Bluetooth AT relay) -- both already load-bearing, real links.
**USART1 is the one peripheral this firmware confirmed is a genuine
no-op stub** (`bx lr`, see the "every real entry point" table above) --
completely unused, on `PA9`. If this exploit is ever attempted, target
firmware built for USART1 avoids any possible pin/bus conflict with
the two real, active links -- worth building for that option first,
not defaulting to USART2 just because it's the most familiar port.
(`PA9`'s physical accessibility on this specific board is unverified,
same open item as `BOOT1`'s pull-up point.)

**Real, concrete failure modes the tool's own docs list** (worth
checking against this board's actual hardware before attempting, not
just noted as a rebuttal if the exploit fails): excess capacitance on
the power/reset lines defeating the glitch timing, the target drawing
enough current to need MOSFET buffering on the Pico's power-control
GPIO, picking the wrong USART peripheral, and the debug probe still
being connected when the glitch fires (it must genuinely disconnect
first, not just idle). None of these are checked/ruled out for this
board yet.

### How this would actually run on this unit's STM32F105RBT6, step by step (2026-08-30)

Mapped directly onto what this project has already confirmed, not the
tool's generic Blue Pill instructions:

1. **Connect SWD** -- same OpenOCD approach already proven working for
   `tools/can-sniffer/`. Known, already-confirmed side effect: holds
   the whole ArkMicro SoC in reset via `GPIOB14` for this phase, screen
   dark, same as every other SWD plan in this project.
2. **Load Stage 1 into SRAM, arm the FPB at `0x00000004`, disconnect
   the debugger.** The FPB redirect is a genuine Cortex-M3 hardware
   feature that survives disconnection -- it doesn't need the debugger
   present to keep working. (Same underlying "inject code into SRAM
   via SWD" capability `docs/MCU_SRAM_TEST_EXECUTION_PLAN.md` already
   relies on for an unrelated purpose.)
3. **Power-glitch**: cut `VDD` to just the STM32, restore exactly as
   `NRST` drops, timed so SRAM retains Stage 1 through the gap. `BOOT0`
   gets toggled to its normal "boot from main flash" state as part of
   this -- nothing about the boot-mode pins looks abnormal to anything
   checking them.
4. **The part that actually matters here**: the CPU's first post-reset
   instruction fetch, which would normally read the real reset vector
   out of this chip's RDP-protected flash, gets silently redirected by
   the still-armed FPB into the SRAM code instead. Per ST's own
   documented RDP1 behavior (already live-confirmed on this exact chip
   earlier this session -- SWD BusFaults the CPU's next flash read
   *while a debugger is attached*), RDP1's block condition simply isn't
   present in this scenario: no debugger, ordinary boot-mode pins.
   Stage 2 then reads real flash as the CPU's own code execution --
   never something RDP1 has restricted; it only gates the external
   debug port.
5. **Stage 2 transmits the dump over a USART.** Per the cross-check
   above, build it for **USART1/`PA9`** -- the one peripheral this
   session already confirmed is a dead stub in the real firmware, so
   there's no contention with the two genuinely active links (`USART2`/
   SoC link, `USART3`/Bluetooth). Capture on the Pico's own UART RX, or
   a plain UART-to-USB adapter plus this project's own capture tooling
   (the same technique class `tools/uart45-probe/` already uses).

**What a real success here would unblock, concretely, not abstractly**:
- **The real, currently-installed Prado-specific firmware** -- not the
  generic `DCn32-VOLVO` reference this project has been stuck
  analyzing all session (confirmed byte-identical to a cross-vendor
  image, not this unit's own firmware -- see the "Important scope
  clarification" section near the top of this doc).
- **Likely the real first-stage bootloader content too**
  (`0x08000000`-`0x08003FFF`), if the dump covers the full addressable
  flash range -- the exact gap the `CMD 0xE1` investigation hit a wall
  on (see that section above).
- **A real backup image**, which would directly unblock `mcu-probe
  --reboot-probe --confirm-erase-risk` -- that command is currently,
  correctly gated specifically because there's nothing to restore from
  if it wipes the chip. This would supply the missing piece.

**What's honestly still missing before this is executable, specific to
this project, not generic tool caveats**: a Pi Pico, a controllable
`NRST` line (current SWD wiring is `SWDIO`/`SWCLK`/`GND`/`VCC` only),
isolated power switching to just the STM32's `VDD` (likely shares a
rail with other board components -- unverified whether that's even
separable without board-level work), physical access to `BOOT1` for
its pull-up resistor, this chip's actual silicon date code (relevant
given the tool's own "may be patched in 2020+ revisions" warning), and
real electrical tuning even once hardware is in hand (the tool's own
documented failure modes -- line capacitance, current draw needing
MOSFET buffering -- are board-specific characteristics that don't
transfer from their reference setup; expect iteration, not a
first-try success).

**Verdict: a real, credible, chip-family-matched avenue, not closed by
anything found this session.** Since it needs the same core hardware
(Pico + `NRST` + switchable power) already on the shopping list for
`racerxdl`, acquiring that hardware would open up trying *both*
techniques in the same session. Not attempted -- recorded here so it
isn't rediscovered from scratch, same as the `racerxdl` entry above.

---

## Real, cheap safe-testing option found (2026-08-30/31): `EliasKotlyar/Canfilter` -- same chip, ~$10, zero risk to the real unit

User shared [`EliasKotlyar/Canfilter`](https://github.com/EliasKotlyar/Canfilter),
a reverse-engineered hardware + firmware project for a commercial
CAN-filter device. Real, confirmed relevance: it's built around the
**exact same chip** as this project's companion MCU --
**STM32F105RBT** -- with **CAN1 on `PA11`/`PA12`**, the standard STM32
mapping this project's own `can_driver.c` already assumes (CAN2 is on
a non-default `PB05`/`PB06` remap on their board, irrelevant here --
this whole project's CAN work has only ever concerned CAN1). Dev
workflow is STM32CubeMX + SW4STM (a real, free Eclipse/GCC-based STM32
IDE); this project's own plain Makefile + `arm-*-gcc` build doesn't
depend on adopting their toolchain, only their hardware.

**Confirmed real and cheap to acquire**: the user confirmed this is a
device purchasable for about $10 -- resolves the one open question
from the initial review (whether this requires fabricating a PCB from
the published design vs. just buying the real thing).

**What this genuinely solves, distinct from both other avenues above**:
a real, physical STM32F105RBT to flash and run this project's clean-room
firmware on, completely separate from the actual Limcet unit -- zero
risk to whatever's really installed there. Specifically better than
the SRAM test-execution plan (`docs/MCU_SRAM_TEST_EXECUTION_PLAN.md`)
for one thing that plan can't do: **real flash-resident execution**,
with correct wait-state timing (the SRAM plan's own honest caveat is
that its zero-wait-state execution runs the calibrated delay loops --
the `GPIOB14` SoC-reset-hold/stabilization sequence -- measurably
faster than intended). It also means the `CMD 0xE1`/YMODEM
bootloader-reflash cycle becomes something genuinely safe to exercise
repeatedly, since there's no real firmware at stake on a $10 spare
board.

**What it can't test, stated plainly -- a different board, not a
Limcet clone.** The Limcet-specific peripherals this firmware's own
`main.c` drives -- `GPIOB14`'s SoC-reset control, the `GPIOC13`/`PC2`
audio/camera-bypass relay, the touch-switch control, whatever the real
board wires to UART4/5 -- simply don't exist on this alternate
hardware. What it validates: CAN1 bus behavior, the UART protocol
logic in the abstract, the `CMD 0xE1`/watchdog-reset/bootloader-entry
mechanism (a chip-level property, not board-specific wiring), and
basic build/toolchain correctness on real silicon. It does **not**
validate the SoC-facing startup sequence or any Limcet-specific GPIO
relay behavior -- the same class of gap already flagged for the SRAM
plan, here because it's the wrong board rather than the wrong memory.

**Real, concrete value as a genuine third option in this project's
"test safely without risking the real unit" toolkit**, complementing
rather than replacing the other two: `picopwner` (above) aims at
*recovering* the real unit's actual firmware; the SRAM plan and this
$10 board both aim at *safely running* this project's own clean-room
replacement, with each covering what the other can't (SRAM: real
board wiring but wrong timing/memory; Canfilter board: real flash
timing but wrong board wiring). Not yet acquired or attempted --
recorded here so it isn't rediscovered from scratch.

---

## Consolidated summary (2026-08-30): every RDP-bypass / backdoor / bug-hunting angle considered this session

Written up per explicit request, pulling together the full sweep across
several sessions of digging into `can_app.bin` for anything beyond the
already-known `CMD 0xA0` settings work. Each row links to its own
section above with the full trace.

### RDP Level 1 bypass attempts

| Angle | Verdict | Why |
|---|---|---|
| `CVE-2020-8004` exception/PC-recovery | **Closed, real** | Live-confirmed on this exact chip: RDP1 BusFaults the CPU's own flash reads while a debugger stays attached (`PC=0x080004AC`, `BFAR=0x080004D4`) -- the earlier `hardware/MCU/live_dumps/` "success" was proven fake and retracted (99.7%+ disagreement vs. the verified-correct image, a real extractor-tool bug). |
| SRAM code-injection / cold-boot (write payload to SRAM via SWD, boot from SRAM instead of flash) | **Closed, by ST's own design** | Verified against ST's own documented RDP1 behavior: flash access is blocked even when booting from SRAM/system-memory while RDP1 is active. No mass-erase risk from trying, just a safe hard fault -- but the technique itself doesn't work on this chip family. |
| `racerxdl/stm32f0-pico-dump` (SWD protocol timing race) | **Open, not attempted** | Attacks debug-port protocol timing directly, not CPU execution -- genuinely different mechanism than the two closed above. Needs hardware not currently on hand: Pico, controllable `NRST`, switchable power. |
| `CTXz/stm32f1-picopwner` (FPB persist-across-reset + power glitch) | **Open, not attempted** | Keeps boot mode at normal "main flash" (unlike the closed SRAM-cold-boot idea) and instead hijacks the reset-vector fetch via a genuine Cortex-M3 debug feature that survives reset. Shares the same missing hardware as `racerxdl`, plus a `BOOT1` pull-up not yet verified accessible on this board. |
| Shellcode-via-UART-overflow (the Medium article that prompted the `picopwner` search) | **Not applicable here** | Requires a pre-existing buffer overflow in the target's own UART parser -- this firmware doesn't have one (see the sweep below). |

### Fabricated / disproven claims found and corrected

| Claim | Verdict |
|---|---|
| `SOC_CMD_DIAG_READ_MEM` (`CMD 0x90`, "Diagnostic Flash/SRAM readback") | **Fabricated, removed from source.** Checked the real dispatch table + its exact bounding loop in 5 real firmware images (this device's own, 2 more byte-identical/near-identical Toyota-generic builds, 1 genuinely different Acura variant) -- appears in none. Would have been a real, working RDP-bypass/full-dump primitive had it existed. |
| `hardware/MCU/live_dumps/README.md`'s claimed successful `CVE-2020-8004` extraction | **Fake, retracted in place.** Git history shows the `.bin` files were committed once then deliberately deleted a session later after being found to disagree with the verified-correct firmware on 99.7%+ of words -- the README was never updated to match, a real documentation-hygiene gap now fixed. |

### Application-level parsing / memory-safety sweep

| Area | Verdict |
|---|---|
| Every real interrupt entry point (UART2/3/4/5, CAN1_RX0, USART1) | **No overflow found.** Checked against the real vector table directly; UART2's initially-suspicious multi-slot ring design traced out to be correctly bounded. |
| Every stack allocation in the entire binary | **No exploitable buffer found.** The two largest candidates (276, 324 bytes) were confirmed false positives -- `objdump` misdecoding inline jump-table data as instructions, not real function prologues. Largest genuine local buffer anywhere is 44 bytes. |
| The one real `strcpy()` call in the firmware | **Safe.** Copies a fixed constant string (`"AT+PIN=0000\r\n"`), not UART-derived data. |
| Length validation architecture | **Centralized and correct.** The UART2 RX ISR enforces `< 28` once, upstream of every handler; every downstream consumer either trusts that pre-validated bound (the buffer is sized to match exactly) or uses its own hardcoded, non-attacker-controlled limit (UART4/5's 5/9/3/9-byte field states). |

### Real bugs found (not security vulnerabilities, but genuine firmware defects)

| Bug | Real-world effect |
|---|---|
| CAN1 RX ring buffer has no "full" detection | Producer (ISR) can silently overwrite unread CAN frames under a 15+ frame burst; if it preempts the consumer mid-read, a torn/corrupted frame can be processed. Could manifest as dropped/garbled steering-wheel or reverse-gear signals under heavy bus load. |
| UART2 (SoC-facing) RX ring has the identical missing check | Same bug, same root cause, on the link `custom_ui`/`tools/mcu-probe` actually use. A software burst of 8+ frames without pacing (easily achievable at 38400 baud) can trigger it -- practical guidance: pace frame sends rather than blasting a batch. |
| `CMD 0x87`'s `"AT+PIN="` digit-substitution bytes are never written anywhere in the reachable firmware | Sits in zero-initialized `.bss` -- the real command almost certainly transmits NUL bytes instead of a valid PIN, a dead/broken vendor feature rather than a working weak-credential backdoor. |

### The one real, standalone security weakness found

| Finding | Real implication |
|---|---|
| `CMD 0x88`'s TEA challenge is a full, stateless decryption oracle (send any 8 bytes, get the plaintext back, no rate limiting) | Combined with the already-documented ~32-bit effective key entropy, this alone -- reachable over the same wire as everything else, zero RDP bypass needed -- would have been sufficient to fully break the "anti-clone" scheme via chosen-ciphertext brute-force. The RDP-bypass effort was never actually necessary to defeat this specific mechanism. |

### Real, benign discoveries along the way

- **UART4/UART5** implement a small, genuine device-identification handshake protocol (sync byte `0x55`, 5 message types) with a real flash-baked identifier string, `"cD31"` -- not Bluetooth as `docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`'s own guess had it (that's confirmed to be `USART3` instead, via its real peripheral base address). Properly bounded, no vulnerability, but a genuinely new fact about this hardware.
- **`CMD 0x85`** was checked as a possible leak candidate and ruled out -- it forwards data to a generic internal 40-slot event queue, never reaching a UART TX path.

---

## Real finding (2026-08-30), continuing the `CMD 0x88` angle: the weak TEA key is a single, shared secret across the whole product line, not per-device

Went back to `CMD 0x88` per explicit request. Given it's a full
decryption oracle with a ~32-bit-effective key (already documented),
the natural next question: is this key unique to this specific device,
or shared?

**Checked directly against all 4 other real firmware images this
project has, by searching for the exact 16-byte key representation
(`6D 00 00 00 7C 00 00 00 A9 00 00 00 C4 00 00 00`) in each binary's
raw bytes:**

| Firmware | Key found? | Offset |
|---|---|---|
| `hardware/MCU/can_app.bin` (this device) | Yes | `0x7cac` |
| `DCn32-VOLVO-V2.10-20240418` | Yes | `0x7bec` |
| `DCn32-VOLVO-V3.00-20240403` | Yes | `0x7c88` |
| `DCn32-ACURA-V1.01-20250409` | Yes | `0x5ff8` |

**The identical key is present in every single one** -- including the
Acura variant, which is confirmed elsewhere in this document to have a
genuinely different command set (7 dispatch entries vs. 9, missing
`CMD 0x84`/`0x87` entirely). This isn't a per-device, per-vehicle-brand,
or even per-firmware-variant secret -- it's one single, global key baked
into the entire "DCn32" product line.

**Real, escalated security implication**: combined with the earlier
finding that `CMD 0x88` is a full, unrestricted decryption oracle
reachable over the same UART link every unit exposes, this means
**extracting or brute-forcing this key from any single unit -- of any
vehicle brand this vendor ships this platform under -- immediately
compromises the "anti-clone" authentication on every other unit of
every brand.** There is no per-device or per-batch key diversification
of any kind. This is a single point of failure for the whole product
family's anti-clone scheme, not an isolated weakness in this specific
Prado installation.

---

## Real finding (2026-08-30), closing the `CMD 0x88` angle: the app-side "anti-clone" check has no consequence at all -- it decrypts, logs, and does nothing else

Followed the key all the way through to its actual use, on the app
side, to answer "what does the anti-clone key help us to do" for real
rather than by inference. Traced `usr/lib/libMcuCenter.so`'s
`MCUAdapter_BoxP300::onRecvMcuProtocol()` (the real function that
receives every inbound MCU->SoC packet on this device, found via
Ghidra headless decompilation, `0x0004bacc`, real ELF vaddr --
Ghidra's own import for this `.so` uses a `+0x10000` address offset
from `nm`'s reported vaddr, confirmed via the already-known
`MCUAdapter_Box_Encryption` constructor address as a cross-check).

**Real dispatch structure confirmed**: the function switches on the
inbound packet's second byte (an opcode, `0x60` for the challenge
*reply*, distinct from `0x88` which is the outbound challenge the SoC
itself sends -- `0x88`/`0x60` are the request/response pair of the same
exchange, not the same byte reused). The `0x60` case:

1. Reads two 32-bit words out of the packet (bytes 3-6 and 7-10) via
   `makeUInt_High()`.
2. Stores them into a fixed global pointer (`puVar29`, resolved through
   a GOT-relative load -- this is `DecryptValues`/`EncryptValues`, the
   same global family the key traces to).
3. Runs the **real, byte-exact TEA decrypt** on those two words, using
   a 4-word key loaded from another global (`piVar27`, `EncryptKeys`)
   -- confirmed matching TEA's real structure (`DELTA=0x9E3779B9`
   equivalent as `0x61c88647` accumulator step, standard Feistel
   `<<4`/`>>5` mixing).
4. Writes the two decrypted words **back into the same `puVar29`
   global** (overwriting the just-read ciphertext with the plaintext).
5. Formats both the ciphertext (before decrypt) and plaintext (after)
   into human-readable debug strings via `QString::number()` +
   `QTextStream`, timestamped, and emits them through Qt's own
   `qt_message_output()` logging path.

**That is the entire handler.** No comparison against an expected
value. No branch on success/failure. No flag set, no feature
enabled/disabled, no session state changed, no error path taken. A
full sweep of every reference to this same global pointer across the
whole decompiled function (`grep` over every line touching `puVar29`)
confirms it is written once (step 2) and read back exactly twice, both
times only to format it into a debug-log string (steps 3's
post-decrypt log and the earlier pre-decrypt log) -- never read by any
conditional. This matches the earlier, independent finding via
`FindDataXrefs.java` (Ghidra's `ReferenceManager`) that nothing else in
`libMcuCenter.so` references `EncryptKeys`/`EncryptValues`/
`DecryptValues`/`EncryptCount` at all.

**Real, definitive answer to "what does the anti-clone key help us to
do"**: on the stock app's own real code path, as it ships today,
*nothing observable*. The MCU sends a challenge, the SoC app decrypts
it, and the plaintext goes straight to a debug log line -- never to an
`if` statement. There is no code anywhere in this library that would
refuse to operate, degrade functionality, or even set a flag if the
decrypted value were wrong. Two honest readings, both worth stating:

- **As shipped, the "anti-clone" mechanism is inert** -- vestigial or
  incomplete instrumentation, not an enforced gate. Cloning a box (or
  reimplementing the MCU side without ever handling `CMD 0x88`/`0x60`
  correctly) would produce no functional difference on the SoC app
  side that this trace can find.
- This does NOT rule out enforcement happening **MCU-side** instead
  (i.e., the MCU itself withholding some other behavior pending a
  successful challenge from its own perspective) -- that would be a
  separate, not-yet-found code path in `can_app.bin`, not in this
  library. Nothing in this session's MCU-firmware disassembly (the
  full 9-entry dispatch table, the `CMD 0x88` handler itself) showed
  any such gating either, but a dedicated re-pass of `can_app.bin`
  specifically looking for "MCU refuses X until challenge succeeds"
  logic has not been done and would be the one remaining way this
  key could still matter functionally.

**Practical upshot for this project**: recovering/knowing this key
does not appear to unlock or need to be replicated for any real
feature this project's own clean-room MCU firmware (`hardware/MCU/source`)
needs to interoperate with the stock SoC app -- `tea_crypto.c` already
implements the real algorithm/key faithfully for protocol completeness,
but there is no evidence the SoC side actually depends on getting a
correct answer back.

---

## Real finding (2026-08-30), verified via Ghidra's Reference API: the "challenge" side is dead too -- `EncryptValues` is generated, then immediately overwritten with a hardcoded constant, and never read again

Re-ran the trace using Ghidra's real `ReferenceManager` API (not manual
reading) against `EncryptKeys`/`EncryptValues`/`DecryptValues`/
`EncryptCount`/`gMcuBoxEnCryption`, once headless analysis had fully
completed (`FindDataXrefs.java`, confirmed `libMcuCenter.so` import
finished cleanly: "Analysis succeeded"/"Import succeeded"). This
resolves real code cross-references the decompiler can follow through
GOT-relative PIC addressing -- something a raw byte search (this
project's usual technique, which works for the bare-metal MCU firmware
but not for a PIC shared library) cannot do at all.

**`DecryptValues`** (the previous section's `puVar29`): confirmed
exactly as traced by hand -- write (`0x4c2f4`, ciphertext in) -> read
(`0x4c500`, pre-decrypt debug log) -> read+read (`0x4c6c4`, fed into
the TEA math) -> write (`0x4c710`, plaintext out) -> read (`0x4c920`,
post-decrypt debug log), all inside `onRecvMcuProtocol`. Verified
address-for-address using a new script
(`DecompileWithAddrs.java`, walks `DecompilerUtils.toLines()` and
prints each line's real instruction address range) against the exact
addresses the reference search reported -- no daylight between the
manual trace and the tool-verified one.

**`EncryptKeys`**: read exactly once (`0x4c6b4`), the TEA key load for
the same decrypt -- consistent with everything already established.

**`EncryptValues` -- genuinely new, and worse than "inert."** Its only
two references are both **writes**, both inside a completely different
function, `MCUAdapter_BoxP300::onInited()` (`0x492d0` -- confirmed via
`getFunctionContaining()`, not guessed). Decompiling that function in
full shows the real sequence, verbatim:

```c
clock_gettime(1, local_90);
srand(local_90[0].tv_sec * 1000 + local_90[0].tv_nsec / 1000000);
iVar3 = rand();
piVar10 = *(int **)(iVar12 + DAT_00049928);   /* EncryptValues */
*piVar10 = iVar3;                              /* real rand() result stored... */

clock_gettime(1, local_90);
lVar1 = (longlong)local_90[0].tv_sec * 1000 + (longlong)(local_90[0].tv_nsec / 1000000);
srand((uint)lVar1 >> 4 | (int)((ulonglong)lVar1 >> 0x20) << 0x1c);
rand();                                        /* ...second rand() result: discarded, unused */

*piVar10     = 0x55a0435b;                     /* ...then immediately overwritten */
piVar10[1]   = 0x4ae4a4eb;                     /* with a fixed, hardcoded constant pair */
```

Two separate `rand()` calls, seeded from real wall-clock time both
times -- genuine, working randomization code, not a stub -- and **both
results are thrown away**. The first is overwritten one instruction
later; the second isn't even stored anywhere. What actually lands in
`EncryptValues` on every single run of `onInited()` (i.e. every app
start, on every unit, since this is compiled-in immediate data, not
config) is the same fixed pair: `0x55a0435b` / `0x4ae4a4eb`.

**And it is never read again.** The reference search's only other hit
for `EncryptValues` is a `DATA` entry (a GOT slot, not code) -- zero
`READ` references anywhere in the whole library. Cross-checked by name
too (`ListFuncs.java` against `hallenge`/`Encrypt`/`Auth`/`Crypto`/
`Clone` substrings): no dedicated "send the challenge" function exists
under any of those names either.

**What this means, stated plainly**: on the real, active adapter class
for this hardware (`MCUAdapter_BoxP300` -- confirmed elsewhere in this
document to be the one actually instantiated and driving the real
`CMD 0xA0` settings UI on this device, not the separate, thinner
`MCUAdapter_Box_Encryption` class this session investigated earlier
before finding it was a dead end of its own), **the "generate a random
challenge" half of the anti-clone scheme has no live wiring either.**
The randomization code runs, produces a real value, and that value is
discarded before anything can use it -- functionally identical to just
writing the two constants directly, except slower. Combined with the
earlier finding (the decrypted *reply* is also never compared to
anything), there is now no discoverable half of this mechanism -- on
either the challenge-generation or response-validation side -- that
does anything beyond writing to a debug log and a global nobody reads.

**Revised, more complete answer to "what does the anti-clone key help
us to do"**: as far as this library's own code goes, it doesn't appear
to help do anything at all, in either direction. Whatever `CMD 0x88`
challenge the MCU actually receives and responds to (confirmed real
and byte-exact on the MCU firmware side, from earlier this session)
does not, on this evidence, originate from -- or get checked against --
this SoC-side global-variable pair. Either the real challenge-send call
site lives somewhere this reference search can't reach (worth a future
broader sweep of every decompiled function in the library, not just
the ones named/reachable from the symbols already known), or this
mechanism is simply vestigial in the shipped SoC app build, consistent
with the app-side and MCU-side halves of this feature having drifted
apart the same way this document has already found for other
`CMD 0xA0`-family features (e.g. the `getSetItemText()`/
`getSetItemValueTexts()` display-name/value-option drift documented
earlier).

---

## Real finding (2026-08-30): `CMD 0xE1` traced end to end -- a real, deliberate reflash-mode trigger, but the actual receiver code isn't in this repo

Continuing the general "can the Linux side reach SRAM/flash" search,
`CMD 0xE1` (`SOC_CMD_REBOOT_BOOTLDR`, "Enter bootloader for update")
was the next real candidate -- it's the one command in the whole real
9-entry dispatch table whose documented purpose is literally "make the
MCU do something with its own flash." Traced both ends, real
disassembly/decompilation on each side.

**MCU side (`can_app.bin`, `CMD 0xE1` handler, `0x80088e0`):** the
handler is a two-instruction shell (`push {r4,lr}; bl 0x80045c4; pop
{r4,pc}`) around the real payload at `0x80045c4`:

```
ldr r0, [pc, #8]   ; r0 = 0x5555AAAA   (a magic cookie constant)
ldr r1, [pc, #12]  ; r1 = 0x20004004   (a fixed SRAM address)
str r0, [r1, #0]   ; write the cookie there
b.n  <self>        ; spin forever
```

Both literal words confirmed by reading the raw flash bytes directly
(not trusting `objdump`'s garbled literal-pool disassembly): `0x5555AAAA`
at `0x80045d0`, `0x20004004` at `0x80045d4` -- `0x20004004` is a real,
valid SRAM address for this chip (STM32F105RBT6's SRAM is
`0x20000000`-`0x2000FFFF`). No `AIRCR` write, no explicit
`NVIC_SystemReset()` -- the handler just writes the cookie and halts.
Given this project's own confirmed-real hardware watchdog
(`project_watchdog_implemented.md` memory), the halt is a deliberate
watchdog-starve: normal operation stops feeding the IWDG, and the
already-configured timeout fires a real hardware reset shortly after.
**SRAM content survives a watchdog reset** (only a power-on/BOR reset
clears it) -- so this magic cookie is specifically designed to still be
readable by whatever code runs immediately after that reset, before
any `.bss`-clearing C runtime init could wipe it.

**Real structural fact, not previously established this session:** this
project's own linker script links the application at `0x08004000`, not
`0x08000000` -- meaning flash `0x08000000`-`0x08003FFF` (16 KB) holds
something else entirely: a genuinely separate first-stage bootloader,
confirmed necessary by this project's own real field-update tooling
(see below), not by the earlier fabricated `live_dumps` claim (that
specific claimed dump of this exact region was fake and retracted --
this section reaches the same "a bootloader region exists" conclusion
completely independently, from the linker script and the real update
flow, not from that discredited source).

**SoC side (`libMcuCenter.so`, real update flow) -- corrects an
assumption from `tools/mcu_builder/README.md`.** That README states
the MCU has a USB port that detects `auto_upgrade.txt` directly; the
real code says otherwise and is more specific. Decompiled the full
chain for the active `MCUAdapter_BoxP300` class (Ghidra, `+0x10000`
offset as established earlier this session):

- `checkMCUUpdateFile()` (`MCUAdapter_BoxP200`, shared base, `0x3d038`)
  -- scans **Linux-mounted disk partitions** (`FileSystemService::
  getDiskPartitionPaths()`) for `auto_upgrade.txt`/the update binary.
  This confirms the trigger file is detected by **Linux userspace
  noticing a USB drive get mounted**, not by any MCU-side USB-host
  logic -- the STM32F105RBT6 in this design has no USB host capability
  used here at all.
- `onStartUpdateMCU()` (`MCUAdapter_BoxP300`, `0x4a8f8`) -- opens the
  found update file, computes the YMODEM block count from its size,
  starts a timer.
- `onSendUpdateReadyTimer()` (`MCUAdapter_BoxP300`, `0x4a3cc`) -- this
  is the real, confirmed sender of `CMD 0xE1` itself
  (`makeMCUProtocol(...,0xe1); ProtocolUtils::writeDatas(...)`), fired
  on a repeating timer -- a periodic "get ready to be reflashed" ping
  to the MCU, not a one-shot command.
- `sendYModemDatas()` (`MCUAdapter_BoxP200`, shared base, `0x3ce0c`)
  -- the actual byte-pump: builds one real YMODEM packet (header byte,
  sequence byte, data block, `CRC16_YMODEM()` checksum) and calls
  `ProtocolUtils::writeDatas()`. **Confirmed one-way send only** -- no
  read/receive call anywhere in this function, no verify-by-reading-
  back step.

**Real, concrete conclusion**: the entire SoC-side update flow, across
all 4 real functions actually driving it, is genuinely a plain,
one-directional file-push mechanism -- Linux detects a USB-mounted
update file, tells the MCU to get ready (`CMD 0xE1`, repeated on a
timer), and streams YMODEM packets. **No read-back, verify-by-reading,
or diagnostic capability of any kind exists in this SoC-side code.**
This rules out `libMcuCenter.so`'s own update logic as a source of any
Linux-side flash/SRAM read primitive -- it was worth checking precisely
because it's the one place in the whole codebase that legitimately
needs to talk about MCU flash content, and it still comes up empty.

**What remains genuinely open, and can't be resolved by more static
analysis**: the actual YMODEM-receiving code that runs on the MCU
after the watchdog reset -- i.e., the real content of flash
`0x08000000`-`0x08003FFF` -- is not present anywhere in this
repository, and there is no trustworthy dump of it (the one claimed
dump was fabricated and retracted earlier this session). This matters
because **RDP Level 1 only gates the external SWD/JTAG debug port** --
it has zero effect on this bootloader's own legitimate code execution,
so if that unknown bootloader's real YMODEM/IAP protocol happens to
implement (or is coerced into implementing, e.g. via a malformed
packet) any read-back, verify, or diagnostic-readout capability, that
would be a completely valid way to read flash/SRAM from the Linux side
with no RDP bypass needed at all -- the same reasoning already
established for why `CMD 0x88`'s decrypt oracle would have mattered,
just aimed at a different, still-unknown piece of code. **This can
only be answered by a real hardware experiment** (not available this
session): trigger `CMD 0xE1` on the real device, then, during the
post-reset window before/instead of sending a real YMODEM stream, send
crafted bytes over `/dev/ttyHS0` and observe whether the bootloader
responds to anything beyond the standard YMODEM handshake (`C`/NAK
polling, `SOH`/`STX` block accept, `EOT` accept). Recorded here as the
single most concrete, well-defined next step for continuing this
whole session's flash/SRAM access search, genuinely blocked on
hardware access rather than more disassembly.

**Real, sharper risk assessment, correcting an earlier understatement
(2026-08-30).** `tools/mcu-probe`'s `--reboot-probe` command (added
this session to run this exact experiment) was initially documented as
merely "reboots the MCU, CAN relay/key-forwarding down for the
outage." That undersold the actual risk, caught and corrected before
anyone ran it on real hardware. Re-checking this project's own
clean-room bootloader reimplementation (`hardware/MCU/bootloader/`,
the only real, buildable stand-in for the unknown vendor code this
project has): its `ymodem_receive_and_flash()` calls
`flash_erase_app_pages()` **before it ever waits for a byte from the
sender** -- erase-then-receive is standard IAP bootloader design, not
an implementation quirk of this reimplementation, so the real vendor
bootloader plausibly does the same thing. That means simply sending
`CMD 0xE1` and never following up with a real YMODEM transfer could be
enough to **wipe the application flash outright**, whether or not
anyone probes for a read-back capability afterward.

**This project has no way to recover from that.** The "have a reflash
ready" mitigation this doc and `mcu-probe`'s own commit originally
suggested is not real insurance: `hardware/MCU/can_app.bin` is the
generic `DCn32-VOLVO-V2.10-20240909` reference build (confirmed
byte-identical to one of the other 4 cross-vendor reference images
this project holds -- see the "Important scope clarification" section
near the top of this doc), not a dump of whatever firmware is actually
flashed on a real physical unit's MCU today, and not confirmed to
decode this specific vehicle's CAN bus correctly if written back.
Writing it over a wiped chip would very plausibly leave real,
vehicle-specific behavior (SWC key codes, reverse-gear/ACC-IGN
triggers, illumination decoding) wrong or broken, not "restored."

**Verdict: do not run `--reboot-probe` (or send `CMD 0xE1` by any
other means) against real hardware until this gap is actually
closed** -- either a genuine dump of the live, currently-flashed
firmware exists, or there's a real, separately-verified answer for
what to flash back if this wipes the chip. `mcu-probe --reboot-probe`
now requires an explicit `--confirm-erase-risk` flag and refuses to
run without it, specifically so this can't be triggered by habit or a
copy-pasted command. This whole `CMD 0xE1` avenue stays open as a real
lead, just correctly gated behind a real prerequisite it didn't have
before.

---

## Real finding (2026-08-30): no bootloader dump exists in any of the other firmware images; this project's own clean-room bootloader has no read capability either, and its commit message doesn't match its own diff

Checked all 4 non-this-device reference firmware images
(`firmware_dumps/MCU/DCn32-VOLVO-V2.10-20240418`,
`DCn32-VOLVO-V3.00-20240403`, `DCn32-VOLVO-V2.10-20240909`,
`DCn32-ACURA-V1.01-20250409`) for a bootloader dump, following up on
the previous section's "the real bootloader isn't in this repo"
conclusion. **None of them are one** -- every file's real size (31804 /
31964 / 31996 / 24624 bytes) is consistent with app-only content
(comfortably under 32 KB), never the ~48 KB+ a combined bootloader
(16 KB, `0x08000000`-`0x08003FFF`) + app (32 KB, `0x08004000`+) image
would need. Same structural conclusion as this device's own
`can_app.bin` -- all 5 real reference images this project has are
app-only.

**A second, real thing surfaced while checking**: this repo already
has its own `hardware/MCU/bootloader/` -- a clean-room STM32F105 IAP
bootloader implementation (`git log`: commit `02b46048`, predates this
session, 2026-08-28), independent of and not previously cross-linked
to this session's own `CMD 0xE1` trace above. Worth checking closely
given the exact coincidence: its `bootloader.h`/`stm32f105.h` already
define `BOOTLOADER_MAGIC_ADDR 0x20004004` / `BOOTLOADER_MAGIC_VAL
0x5555AAAA` -- byte-exact matches for the values this session
independently re-derived via fresh disassembly in the previous
section. This isn't a new discovery colliding with an old one by
chance -- it's confirmation that an earlier, unsummarized pass of this
project already did this exact `CMD 0xE1` disassembly (also recorded,
found while checking, in `hardware/MCU/MCU_FIRMWARE_REVIEW.md`,
`docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md`, and
`docs/historical/1.3_MCU_ADAPTERS.md` -- all three already had this same magic-
cookie/watchdog-reset finding on record). This session's independent
re-derivation in the previous section is real and address-verified,
just not novel -- flagged here so it's not miscredited.

**Read the actual bootloader source** (`main.c`, `ymodem.c`) to check
whether it implements any memory-read capability. It does not:
`main.c`'s entire flow is `check magic -> jump to app` or `run
ymodem_receive_and_flash() -> jump to app -> else AIRCR system reset`;
`ymodem.c` implements exactly `flash_unlock()`/`flash_lock()`/
`flash_erase_app_pages()`/`flash_write_page()`/the YMODEM receive loop
itself (`SOH`/`STX`/`EOT` framing, CRC16) -- a plain write-only IAP
receiver, structurally consistent with the one-way `sendYModemDatas()`
push already traced on the SoC side. No read-flash/read-SRAM function
exists anywhere in this file.

**Real, honest discrepancy worth recording**: the commit that added
this bootloader is titled *"add cleanroom STM32F105 IAP bootloader and
diagnostic memory read command (0x90)"* -- but its actual diff touches
only `hardware/MCU/bootloader/*`, and grepping that diff and the
current file contents for `0x90`/`DIAG`/`diagnostic`/`READ_MEM` finds
nothing at all. The "diagnostic memory read command (0x90)" the
message describes was never actually implemented in this bootloader --
most likely a stale/copy-pasted commit message from the same session
that (per `git log -S`) also touched the app-side `SOC_CMD_DIAG_READ_MEM`
definition this session already found, confirmed real across zero of 5
firmware images, and removed as fabricated (see the "CMD 0x90 --
disproven" section above). Whatever the cause, the practical fact
matters more than the label: **this project's own clean-room
bootloader, as it stands today, has no read-flash or read-SRAM
capability of any kind, by direct inspection of its complete real
source** -- consistent with, not contradicting, the app-side `CMD 0x90`
finding.

**Net answer to "is there a bootloader dump/read-capable bootloader
anywhere in this project"**: no, on both counts checked. The 4 other
real reference firmware images are all app-only, and this project's
own clean-room bootloader (the only "bootloader" that exists anywhere
in this repo as real, buildable source) is a plain write-only IAP
receiver with a commit message overclaiming a diagnostic feature that
was never actually written. The genuinely unknown quantity remains
exactly what the previous section already identified: the REAL vendor
bootloader that ships on actual hardware at `0x08000000`-`0x08003FFF`
-- still not present in this repo, still only resolvable by a live
hardware experiment, not by anything checkable in source.

---

## Real finding (2026-08-30): what information actually transits the UART4/5 link -- traced as far as static analysis can go

Following the field-size correction and the discovery of the external
`0x20`-field-updating function (both above), asked directly: what real
information moves over this link, in each direction, and how far can
it be traced?

### Outbound (MCU -> peer) -- two genuinely different kinds of data

- **Type `0x32`'s field is a fixed, hardcoded identifier**, confirmed
  at its real flash source location: the string `"   cD31"` (three
  spaces then `cD31`) plus a NUL and a trailer byte `0x93` lives at
  `0x0800BBCD`-`0x0800BBD5`, immediately preceded (at `0x0800BBC8`) by
  the tail end of the CAN Mode-1 dispatch table (its last entry,
  `0x035 -> 0x0800ABC9`) -- confirming this data sits in the firmware's
  general-purpose "static tables and constants" region, not somewhere
  suggesting live/derived content. This is genuinely just "here's what
  kind of module I am" -- a real identification string, nothing more.
- **Type `0x20`'s field is not fixed** (see the prior section) -- it
  reflects live struct data (offsets `+4`/`+8`) that an external
  function refreshes, and that same function also posts real SWC
  key-press/release events for other input patterns. The *real-world
  meaning* of struct `+4`/`+8` themselves was not resolved this pass
  (see below for why) -- what's established is that this field is
  genuinely dynamic and tied to the same subsystem as steering-wheel
  key decoding, not that its specific numeric meaning is known.

### Inbound (peer -> MCU) -- accumulated, but provably never used

**Definitive, not just "not found yet":** the struct base address
(`0x20001365`) has exactly 4 references anywhere in the entire 32KB
firmware image -- the flash-populate function (`0x80065b4`), UART4's
own ISR, UART5's own ISR, and the external `0x20`-field-update function
found this session. All 4 were individually examined. **None of them
read** the three inbound fields (`0x50` at `+0x4e`/3 bytes, `0xD3` at
`+0x3f`/9 bytes, `0xD6` at `+0x5d`/9 bytes) after the UART ISR
populates them -- the update function only ever *writes* into `+0x21`
(the `0x20` field), never reads from `+0x3f`/`+0x4e`/`+0x5d`. Since a
literal-pool cross-reference search is exhaustive for this struct base
(any code touching these offsets would need to load this same base
address first, and all such loads were enumerated), this is a clean,
provable negative: **whatever a real peer sends via types `0x50`/
`0xD3`/`0xD6` is accepted, stored, and never acted on anywhere in this
firmware image.** Matches a recurring pattern already documented
elsewhere in this codebase (`CMD 0x87`'s PIN-substitution bytes, the
app-side `CMD 0x88` anti-clone check) -- write-only/dead-end data
paths are a real, repeated characteristic of this firmware, not a
one-off.

### Where the trace genuinely stops

The external `0x20`-field-update function (`0x800b8a0`) is invoked
through a function pointer stored in a small data table at
`0x800bbf0`, immediately following the flash-resident field-source data
-- confirmed via a direct hit (`0x800b8a1`, the Thumb-bit-set form)
found in that exact table slot. **No code anywhere in the firmware
references this table's address as a literal** -- ruling out the
simple "fixed address, direct call" pattern this project's disassembly
technique (literal-pool cross-referencing) handles well, and pointing
instead to a runtime-computed table walk (a base address plus a
variable index/stride), which this technique can't resolve without a
much deeper, dedicated trace of whatever computes that index. Not
pursued further this pass -- a real, bounded next step if this
specific question (what real-world trigger refreshes the `0x20`
field, and what do struct `+4`/`+5`/`+8` concretely represent) is
worth the additional effort. The cheaper alternative, if this is
wanted: a live hardware test -- query type `0x20` on real hardware
repeatedly over time/across different vehicle states and see whether
the returned bytes ever change, which would confirm the "live, not
static" finding empirically without needing to fully resolve the
table-walk in source.

---

## `flag_5e`, fully traced (2026-08-31) -- and a real correction to this project's own GPIO pinout table found along the way

User asked whether the corrected camera-type toggle (`CMD 0xA0 id=0x11`,
see the `custom_ui` fix this session) would actually stick, which
turned on whether `id=0x11`'s own gate condition (`flag_5e`, this
project's `hardware/MCU/source/include/uart_protocol.h` struct comment,
previously "Real firmware sets this elsewhere; not traced in this
pass") could ever become true. Traced it completely.

### The real chain

1. **A real polling function** (entry `0x080084A4`) reads **five real
   GPIO input pins** into a packed status byte, via a single shared
   helper (`0x08005582`) that does `ldr r3,[r2,#8]` -- the STM32 `IDR`
   (input data register) offset, confirmed to be a genuine input read,
   not an output/control write:
   - bit 0 = `GPIOA Pin 8`
   - bit 1 = `GPIOC Pin 9`
   - bit 2 = `GPIOC Pin 8`
   - bit 3 = `GPIOC Pin 7`
   - bit 4 = `GPIOB Pin 2`
2. The packed byte is change-detected against a stored "last known"
   value (`0x080085D8`) -- the function returns immediately if nothing
   changed across any of the five pins.
3. If something changed, `bit 4` (`GPIOB Pin 2`) is separately
   extracted, **inverted**, and stored as its own tracked value at SRAM
   `0x20000066`, which is *itself* change-detected against its own
   "last known" byte (`0x20000067`).
4. Only when *this* inverted-bit-4 value newly changes:
   - New value `0` (i.e. `GPIOB Pin 2` reads HIGH) -> `flag_5e = 0`,
     calls `FUN_080058A4(0)`.
   - New value `1` (i.e. `GPIOB Pin 2` reads LOW) -> **`flag_5e = 1`**,
     then branches on `id=0x11`'s own stored setting value to call
     `FUN_080058A4(2)` or `FUN_080058A4(3)`.

**Real, precise answer**: `flag_5e` becomes `1` -- enabling `id=0x11`'s
real `GPIOC Pin 13` camera/video-relay effect -- specifically when
**`GPIOB Pin 2` transitions to reading LOW**. What `GPIOB Pin 2`
physically senses on this board (a strap, a connector signal, something
else) is **not independently confirmed** -- this is the disassembly-
verified trigger *condition*, not yet its real-world *source*. A real,
concrete next step if this is worth resolving further: check this
board's schematic/silkscreen for what's wired to `GPIOB Pin 2`, or
probe it directly on real hardware.

**HARDWARE-CONFIRMED (2026-09-01): the switch really is fully
autonomous.** User ran `killall custom_ui` on real hardware -- leaving
no process at all to send or poll anything over the MCU UART -- and
reverse gear still switched to the OEM factory camera feed correctly.
This directly confirms the disassembly above: the MCU's own `GPIOB
Pin 2` edge-poller drives the `id=0x11` relay entirely on its own once
the preference is armed. `GPIOB Pin 2`'s physical wire identity is
still unconfirmed, but "no software involvement needed" is now a real,
observed fact, not just what the disassembly predicts.

**RESOLVED (2026-08-31), hardware-confirmed (2026-09-01): the
reverse-gear-triggered `id=0x11` resend was removed entirely.**
`custom_ui` used to resend `id=0x11` on every reverse-gear transition
(re-arming `1` on engage, forcing `0` on disengage). The disengage-
forced-`0` send unconditionally de-armed the factory-camera preference
on *every* disengage -- meaning OEM Factory Camera mode only ever
worked for the first reverse-gear engagement after boot or a settings
change, then silently broke on every subsequent engagement, since
nothing re-armed it except that same code's own engage branch (itself
just re-sending an already-armed value every time -- real,
self-inflicted churn). The user reported, from direct real-world
experience, that reverse-camera switching worked *better* before these
sends existed at all. `main.cpp`'s reverse-gear handler no longer
sends `id=0x11` reactively at all (`custom_ui` commit `a4211c2`); the
preference is armed only at boot and immediately on every settings
change. Real hardware retest (2026-09-01): multiple full engage/
disengage cycles all worked correctly, zero regression -- confirmed
the simplification holds on real hardware, not just at build time.

### Real, wider correction found along the way -- this project's own GPIO pinout table had 4 wrong rows, not just the one relevant to `flag_5e`

Tracing the five input pins above meant resolving their real port+pin
identities precisely -- and 4 of the 5 addresses involved (`0x0800599C`,
`0x080059B0`, `0x080059D8`, `0x080059E8`) turned out to be **exact
matches** for rows already in this project's own long-standing GPIO
pinout table (`docs/historical/1.3.1_MCU_FIRMWARE_DECOMPILATION.md` §7, copied
into `docs/HANDOFF_MCU_AUDIO_I2C.md` too) -- and all 4 were wrong, both
on **port** and on **function type**:

| Address | Table claimed | Real (confirmed via `IDR` read + literal-pool GPIO base) |
|---|---|---|
| `0x0800599C` | `GPIOA Pin 1`, "Power Amp Mute" (output) | `GPIOA Pin 8`, input read |
| `0x080059B0` | `GPIOA Pin 9`, "USB 5V Power Rail" (output) | `GPIOC Pin 9`, input read |
| `0x080059D8` | `GPIOA Pin 7`, "AM/FM Radio Tuner Power" (output) | `GPIOC Pin 7`, input read |
| `0x080059E8` | `GPIOA Pin 2`, "Bluetooth Module Power" (output) | `GPIOB Pin 2`, input read (the real `flag_5e` source) |

All four are unambiguous by construction -- `ldr r3,[r2,#8]` is the
`IDR` offset on every real STM32 GPIO peripheral, architecturally
impossible to also be an output-control function. The old table's
port/pin *and* function-type labels for these four rows were both
wrong, not just stale naming. **Corrected in both docs, with the same
"the other rows weren't re-checked this pass" caveat** -- this is a
real, scoped correction of exactly the four addresses independently
re-derived, not a blanket retraction of the whole table. Worth
treating any row in that table as needing a fresh check before relying
on it for something new, given this.

## Real finding (2026-09-02): the firmware genuinely transmits CAN frames -- not receive-only, and it targets CAN2, not CAN1

Prompted by a real U0073 (Control Module Communication Bus Off) code
logged during regular driving, with no `custom_ui`/reverse-gear/camera
activity around it -- the user asked whether this project's MCU work
could have caused CAN packet conflicts. Investigated by direct,
from-scratch disassembly of `hardware/MCU/can_app.bin`, not assumption.

### The transmit function itself, fully traced

A complete `CAN_Transmit()`-equivalent function at `0x08004CD2`
(`CAN_Transmit(CANx_base, TxMessage*)`):

1. Reads the real `TSR` register (`base+0x08`) and tests the three
   mailbox-empty flags (`TME0`=bit26, `TME1`=bit27, `TME2`=bit28) to
   pick a free mailbox index (0/1/2), or returns immediately with no
   transmission attempted if all three are busy (`r0==4` -> early
   `pop {r4,pc}`) -- matches real `CAN_Transmit()`'s `CAN_NO_MB` path
   exactly.
2. Computes `&sTxMailBox[idx]` as `base + 0x180 + idx*0x10` -- the
   real STM32 `CAN_TypeDef` offset for the TX mailbox array, same
   `0x1B0`-family addressing style already confirmed for the RX FIFO
   side of this firmware.
3. Packs the caller's message struct into `TIR`/`TDTR`/`TDLR`/`TDHR`:
   standard-ID path shifts `StdId` into bits `[31:21]`, extended-ID
   path shifts a 32-bit `ExtId` into bits `[31:3]`, `RTR`/`IDE` bits
   set accordingly, `DLC` written into `TDTR` bits `[3:0]`, 8 data
   bytes packed into `TDLR`/`TDHR` -- byte-exact match to the real
   hardware register layout.
4. **The last write, unconditionally: reads `TIR` back, ORs in bit 0
   (`TXRQ`), writes it back.** This is the literal hardware trigger
   that makes the bxCAN peripheral transmit the frame onto the wire.
   There is no simulation/loopback framing around this -- it's the
   real register write real STM32 HAL code uses to fire a
   transmission.

This settles, with direct evidence, something this project's docs had
left open: the firmware is not receive-only. It actively drives the
CAN bus.

### Confirmed genuinely called, not dead code -- and it targets CAN2

Found 2 real call sites via a from-scratch Thumb2 `BL`-instruction
decoder (a proper 32-bit `BL` bit-pattern decode across every 2-byte
file offset, not a linear-sweep capstone guess, which desyncs badly
around embedded literal pools in this binary):

- `0x0800800E` -- inside a periodic, table-driven routine: a rotating
  index byte (`+0x12D` of a small SRAM state struct at `0x200002BC`,
  wrapping mod 15) selects one of 15 pre-built 20-byte message
  templates stored inline in that same struct (`base + index*20`,
  0-299 bytes, with the index counter itself living right after at
  offset `0x12C`/`0x12D` -- the struct is sized exactly for 15
  entries, no coincidence), `memcpy`s the selected template to a
  stack buffer, then calls `CAN_Transmit(CAN2_base, &stack_msg)`.
- `0x0800809C` -- the counter-increment function itself (mod-15
  wraparound, matches the earlier `0x08007078`-family functions
  already covering the CAN1 side).

**The CAN base literal at this call site is `0x40006800` -- CAN2, not
CAN1 (`0x40006400`)**, the peripheral the previously-confirmed RX-frame
unpacking code reads from. This is a new, precise data point for this
project's long-open "which peripheral is actually live, CAN1 or CAN2"
question (see the `CMD 0x12`/CAN-pin sections elsewhere in this doc and
in `docs/MCU_COMMAND_REFERENCE.md`) -- the real answer now looks like
**both, on different roles**: CAN1 receiving, CAN2 transmitting (or at
minimum, this one confirmed TX call site uses CAN2; not yet checked
whether anything also transmits on CAN1).

### What this does NOT establish -- stated plainly, not glossed over

- **The actual IDs/payloads transmitted are not recoverable from the
  static binary.** The 15-entry template table lives in SRAM
  (`0x200002BC`), populated at runtime -- there's no corresponding
  data in the flash image to read. Attempted to cross-reference against
  `tools/mcu_builder/mcu_decompile.py`'s already-documented "Mode
  1/2/3 (Profile 1/2/3)" flash tables (`0x0800BAE8`/`0x0800BB30`/
  `0x0800BB80`) as a plausible source, applying this transmit
  function's own confirmed 20-byte struct layout (`StdId` u16 @0,
  `ExtId` u32 @4, `IDE`/`RTR`/`DLC` u8 @8/9/10, 8 data bytes @11) --
  produced implausible values (`IDE`/`RTR` bytes far outside 0/1,
  `ExtId` fields that look like flash code addresses, e.g.
  `0x0800990D`). **That lead doesn't hold up and is not claimed as the
  real source** -- the RAM table's actual populating code hasn't been
  found yet, real open question, not answered here.
- Whether CAN1 also has an active transmit path (this pass only
  confirmed CAN2's) is unchecked.
- Whether this CAN2 transmission actually reaches the real vehicle bus
  at all depends on the same still-open physical-wiring question the
  rest of this project has been chasing: which physical pins/
  transceiver this MCU's CAN peripherals are actually wired to on real
  hardware (`PA13`/`PB15`/`PC6` don't match either bxCAN's real
  hardware remap pin pairs -- see `docs/MCU_COMMAND_REFERENCE.md`).
  Nothing here resolves that; if anything it raises the stakes of
  resolving it, see below.

### Answering the real question this was prompted by: is `custom_ui`'s MCU work implicated in the U0073 (Bus Off) code?

**No, and not just as reassurance -- structurally not possible from
what's confirmed here:**

1. `custom_ui`/`androidauto-sidecar` communicate with the MCU
   exclusively over its dedicated UART (`ttyS2`, the `0x2E`-framed
   protocol) -- neither has any code path onto the CAN bus, direct or
   indirect.
2. This project has never modified or reflashed this MCU's firmware --
   everything traced above is stock behavior, present and running
   regardless of anything `custom_ui` does or has ever done.
3. The transmit path found here is **periodic and table-driven**,
   firing on the MCU's own internal cycle independent of any UART
   traffic from the SoC -- exactly consistent with the user's own
   report of "regular driving, no specific triggers": there wasn't a
   software-side trigger to find, because this transmission runs as
   continuous background behavior either way.

**The real, honest safety implication, reframed by this finding**:
this MCU is now confirmed to be an *active* CAN participant, not a
passive listener -- which makes the project's still-unresolved
physical CAN wiring/transceiver question (`PA13`/`PB15`/`PC6` vs. the
real bxCAN hardware pins) more consequential than it looked before, not
less. A hardware-level fault in that connection (a bad tap, wrong
termination, a bit-banged/hardware-CAN timing mismatch against the
real vehicle bus) is a genuine, concrete candidate mechanism for a
real Bus-Off condition on this vehicle -- but it is a **hardware/
firmware-design question, orthogonal to anything in this repo's
software**, not a regression this project's own changes could have
introduced.

**Practical next step, if this is worth pursuing further**: real,
on-vehicle CAN bus health data (`docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md`
already has a real, ready-to-run procedure for this) would show
whether this MCU's own CAN2 transmissions are well-formed on the real
bus, or whether something at the physical layer is actually degrading
them -- the only way to move this from "plausible mechanism" to
"confirmed cause," which this static-analysis pass alone cannot do.

### Real corroborating field data (2026-09-02): U0073 freeze frame from the actual vehicle

User supplied the real Toyota freeze-frame data for the logged U0073.
Two things worth recording:

- **Parameter set identifies the reporting module**: wheel speeds, yaw
  rate, lateral/forward G, steering angle, master cylinder pressure,
  TRC/VSC mode -- this is classic Skid Control ECU (ABS/VSC) freeze-
  frame data. The DTC was very likely logged by the ABS/VSC module
  noticing *its own* CAN controller drop off the bus -- a genuine,
  independent module's view of a real bus-wide health problem, not
  something isolated to the aftermarket head unit's own diagnostics.
- **Driving conditions at the fault, ruling out a reverse-gear
  correlation directly rather than just by absence of report**: 4th
  gear, `Shift Lever Position: D/M` (forward), steady 44 km/h, mild
  cornering (`Steering Angle` climbing `-0.1 -> 3.2 -> 5.7`deg`, `Yaw
  Rate` just starting to register), slight throttle lift and mild
  engine-braking torque (`-1.1 -> -26.0 -> -35.6 Nm`), `TRC/VSC Off
  Mode: Normal`. An ordinary gentle turn/lift-off -- reverse gear
  provably not engaged (`Gear Position: 4th`), so this specific event
  cannot be tied to the reverse-gear/camera-relay UART work at all,
  not merely "no trigger reported."

Consistent with, not just uncorrelated to, the physical-layer/wiring
theory above: an independent vehicle module reporting a bus-off during
otherwise unremarkable driving fits a hardware-level cause (marginal
tap, intermittent contact, timing mismatch) better than any
event-triggered explanation, software or otherwise.

### Real correlation found (2026-09-02): the flat battery lines up with the SWD-drain finding -- now the leading explanation, not the physical CAN wiring theory

User connected `Number of IG On = 5` in the freeze frame (an ignition-
cycle counter) to a real, remembered event: **the vehicle's battery
went flat five ignition cycles before this DTC was read** -- and
confirmed that flat battery was the one from leaving an SWD debugger
connected, already documented above in "Real, user-observed finding
(2026-09-01): SWD-attached battery drain."

This matters because a flat battery, and specifically the voltage sag
that happens during and after recovering from one (jump-starting,
reconnecting a charger, the alternator re-establishing normal voltage),
is one of the most common, well-understood real-world causes of a
`U0073`-class Bus-Off DTC -- low/unstable supply voltage pushes CAN
transceivers' common-mode levels out of spec and can brown-out an
ECU's own controller mid-communication, both of which spike a node's
transmit/receive error counters and can trip Bus-Off on the affected
module (here, very likely the ABS/VSC ECU itself, not this project's
MCU). This is a genuinely mundane, textbook mechanism -- it does not
require the CAN2-transmit or physical-wiring theories above to be
wrong, but it's a simpler, better-evidenced candidate given the direct
event correlation, and should be treated as the leading explanation
unless something rules it out.

**Real, honest reframing of "is this project's work implicated"**:
the answer is still no for a *software/packet-conflict* mechanism (see
above -- `custom_ui` has no CAN-bus code path, full stop), but **yes,
indirectly, through a completely different and already-documented
mechanism**: an SWD debugging session run as part of this project's own
CAN/reverse-relay investigation work drained the battery flat (per the
2026-09-01 finding), and that flat-battery event -- not any UART
command, not any CAN2 periodic transmission -- is the most likely real
trigger for this specific DTC, via ordinary voltage-sag brownout, not
packet conflicts. Worth stating plainly rather than only defending the
"software isn't the cause" framing: the SWD battery-drain risk already
flagged as a practical concern for future hardware sessions had a real,
concrete consequence this time, not just a theoretical one.

## Real finding (2026-09-02): `/dev/carback` is dead by deliberate design on this product, not a probe-order bug -- and the real signal it names is still unidentified

Investigated why `/dev/carback` is unavailable at runtime, per the user's request to look
into reviving it as a cleaner reverse-gear signal (see `docs/MCU_COMMAND_REFERENCE.md`'s
`CMD 0x12` sections for the context this came from).

**The kernel driver itself (`linux/drivers/soc/arkmicro/ark-carback.c`) is fully intact and
functional** -- real `probe()`, real `cdev`/`class`/`device_create()` sequence, a genuine
2026-08-03 fix for a `g_carback`/`itu656` probe-order race (both sides checked directly:
`ark_carback_probe()`'s own corrective `carback_first_enter()` call, gated on
`ark1668_itu656_is_probed()`, and `itu656`'s own unconditional call at probe end -- the
fix is real and correctly resolves that specific race, the "g_carback null error" warning
seen in boot logs is a harmless one-time artifact of it, not a sign of failure).

**The real, actual cause is upstream of the driver entirely**: `ark1668_limcet_p305.dts`
(this exact product's real devicetree) has **no `ark-carback` platform device node at
all** -- removed 2026-07-17, with a real, dated, already-existing comment explaining why:

> "Reversing signal detection -- NOT a SoC GPIO on this product... the real kernel's
> `carback` platform_device has an all-zero platform_data struct (no GPIO baked in), and
> MsnCoreApp's active MCU adapter for this product (`MCUAdapter_BoxP300`, `McuType=6`)
> never touches `GPIOOperater` for it -- reverse-gear detection happens entirely on the
> companion STM32F105 MCU... which just sends a `backcar enable/disable` command to the
> SoC over the existing HS-UART arktool link"

So `/dev/carback` isn't a bug to fix or a race to win -- **the node was correctly removed
because it was never real on this product**: its old `detect-gpios` pin (`&gpio0 5`) was
actually pinctrl pin 5, silently conflicting with LCD `r3`, for a GPIO signal that never
existed for this purpose in the first place. Reviving it would mean re-adding a real
display-pin conflict for a signal path this project's own prior work already confirmed is
not how this product detects reverse gear. **Not a viable path -- closed, not deferred.**

**The real path forward, per this same comment, is confirming the actual `backcar
enable/disable` command** -- which, given today's finding that `CMD 0x12` is a hard no-op
in the real vendor app for every payload this project has ever captured (see
`MCU_COMMAND_REFERENCE.md`), is very likely **not** `CMD 0x12` after all. Searched
`libMcuCenter.so` for real string evidence: found genuine candidate vocabulary --
`"Reverse State"` (`0x95d64`), `"Reverse Condition"` (`0x95db0`) -- distinct from the
already-understood `"Reverse Track"`/`"Reverse Radar"` strings (`CMD 0x0A`/`CMD 0x04`).
**Real dead end reached, not glossed over**: neither string has a findable direct code
xref via either a MOVW/MOVT absolute-address pattern or a literal-pool pointer scan (both
techniques that worked cleanly for other real fixes traced this session) -- consistent
with this project's own already-documented caveat elsewhere ("Qt translation catalog
confirms vocabulary, doesn't resolve remaining ids"): these are very likely Qt
translation-resource strings (UI label text), not something with a simple direct pointer
reference from the C++ code that would consume them. **Not resolved** -- the real
"backcar enable/disable" command this DTS comment refers to remains unidentified.

## Real finding (2026-09-02): a genuine SoC-GPIO car-signal watcher exists (`CarSignalsWatch`) -- but it's audio/Bluetooth-related, not the "backcar" reverse-gear command

Continued digging for the real "backcar enable/disable" command the DTS comment names
(see the `/dev/carback` section above). Found a real, previously-undocumented class in
`MsnCoreApp` itself: `CarSignalsWatch` (constructor `0x49b58`) -- watches exactly two SoC
GPIOs directly via `GPIOOperater`/epoll (**GPIO 30 and GPIO 31**, hardcoded), completely
independent of the MCU-UART link. This is a real, generic mechanism, not specific to
carback -- worth recording since it wasn't known to exist before this pass.

Traced the full path: a value change on either GPIO posts `MsnEvent` type `0x5004` (GPIO
value != 1, "state added") or `0x5005` (GPIO value == 1, "state removed") carrying the
GPIO's own `getTag()` value as parameter -> `MsnCoreApp::msnAppNotify()` real dispatch
(confirmed via direct trace, not the earlier "no confirmed consumer" dead end that applied
to `0x5026`) -> `MsnCoreApp::addAppStates(u64)` / `removeAppStates(u64)`, a generic 64-bit
vehicle-state bitmask.

**Real, specific bit meanings identified within that bitmask** (via `addAppStates`'s own
branch logic):
- bit 24 (`0x1000000`) -> `MsnWindowManager::appStateChange()` -- a real UI state-change
  response
- bit 25 (`0x2000000`) -> gated on `MsnApplication::isSoftBluetooth()`, then either
  `appStateChange()` or writes back to a different `GPIOOperater` -- Bluetooth-related
- bits 26/27 (`0x4000000`/`0x8000000`) -> both call `SoftVolCtrl::setVolume(index, true)`
  -- audio volume

**None of these are reverse-gear related.** Could not determine which specific bit(s)
GPIO 30/31 themselves set -- no `setTag()` call found anywhere in `CarSignalsWatch`'s own
methods, so `getTag()`'s real return value (and therefore the exact bit) is unresolved.
But the bits that *are* identifiable all point toward audio/Bluetooth hardware switches,
not reverse gear -- consistent with, not contradicting, the DTS's own claim that reverse-
gear detection isn't SoC-GPIO-based on this product at all. **Working read: `CarSignalsWatch`
is very likely NOT the "backcar" mechanism** -- it's a real, separate, previously-unknown
audio/BT-switch watcher, a genuine new finding on its own merits, but not the lead this
search was chasing. The real "backcar enable/disable" command referenced in the DTS
comment remains unidentified after this pass.

## CORRECTION (2026-09-02): the 2026-08-29 "full settings-name resolution" pass had `idx=0`/`idx=1` swapped -- `id=0x00` is the real Camera setting, not `id=0x01`

Prompted by cross-checking a separate "other agent" report's settings table (which claimed
`id=0x00` = "Microphone Type" and directly contradicted the real hardware test just
completed on `id=0x00` -- see `docs/MCU_COMMAND_REFERENCE.md`'s `id=0x00` section) --
independently re-disassembled `MCUAdapter_BoxP300::getSetItemValueTexts(int)` directly
(`0x00032460` in `firmware_dumps/Prado firmware dump/mtd6_rootfs/usr/lib/libMcuCenter.so`,
confirmed via real exported symbols, not guessed), tracing the real jump table and each
`QMetaObject::tr()` string argument byte-for-byte.

**Real, confirmed result**: `getSetItemValueTexts(0)` (i.e. `idx=0`, `CMD 0xA0 id=0x00`)
returns exactly `["AfterMarket Camera", "Factory Camera", "AfterMarket 360", "Factory
360"]`. `getSetItemValueTexts(1)` (`idx=1`, `id=0x01`) returns an **empty list** -- the same
generic no-named-values fallback shared with `idx` 2 through 6, not a combobox at all.

**This is the exact opposite of what the earlier 2026-08-29 pass documented** (which
attributed the 4 camera strings to `idx=1` and the "OEM Microphone"/"AfterMarket
Microphone" strings to `idx=0`) -- a real, now-corrected indexing error in this project's
own prior analysis, not a vendor bug. Given today's real, methodical hardware test already
independently confirmed `id=0x00` controls the OEM camera relay (see
`docs/MCU_COMMAND_REFERENCE.md`), this correction means there's no vendor label-vs-function
mismatch to explain at all: `id=0x00` is consistently, correctly the camera setting by both
name and real function. The earlier "stock's own internal label for this id is 'Reversing
camera', out of sync with the microphone-sounding value-text strings" framing (used to
justify keeping the now-removed standalone "Microphone Source" toggle) was built on the
same underlying indexing mixup and should be read in that light -- not retracted outright
(the `getSetItemText()` vs `getSetItemValueTexts()` divergence it described may still be
real for some other id), but the specific `id=0x00`-is-secretly-camera framing is now fully,
independently explained without needing it.

**Also worth recording**: `id=0x09` (`OEMMicrophone` in `custom_ui`'s own settings, "OEM
Factory Microphone" toggle) is a completely separate, already-independently-confirmed real
setting, untouched by this correction.

**Not yet re-verified**: `getSetItemText(0)`/`getSetItemText(1)` (the UI *label* strings,
as opposed to the value-texts just traced) weren't re-disassembled in this pass -- only
`getSetItemValueTexts()` was directly checked. If precision on the real on-screen label
stock itself shows for `id=0x00` matters for future work, that still needs its own direct
re-trace rather than trusting the 2026-08-29 pass's claim about it.

## New library found while chasing `CMD 0xFF`'s sender: `libCanBus.so` -- a whole separate multi-brand CAN-dashboard subsystem (2026-09-03)

Not previously opened by this project. Contains a real, generic CAN-bus dashboard-integration
plugin (`CanBusPlugin`) with **4 distinct vehicle-brand adapter classes** sharing one
`libCanBus.so`: `CanBus_XinRi`, `CanBus_Raise_Volkswagen`, `CanBus_XBS_Mazda`, `CanBus_
LiHang_JMCE200N` -- each with the same real method-set shape as `libMcuCenter.so`'s
`MCUAdapter_*` family (`onRecvMcuProtocol`, `onRecvAppProtocol`, `makeCanBusProtocol`,
`writeCanBusData`, `getPortSettings`, etc.), and confirmed to use the identical `0x2E`
sync-byte wire framing (`mov r3,#0x2e` found directly inside `makeCanBusProtocol()`). Real
Qt resource names embedded in the binary (`dazhong_canui`, `canbusimages`) confirm this is
generic aftermarket/OEM CAN-dashboard integration for several vehicle brands, none of which
is this product's own Prado/`BoxP300` build -- same "bundled but inactive for this SKU"
pattern already established for `libMcuCenter.so`'s many `MCUAdapter_BoxPxxx`/`BoxCxxx`
variants.

Checked all ~20 real `writeCanBusData()` call sites across all 4 classes for `cmd=0xFF`
(255) via an immediate-constant argument -- none found. Real, honest caveat: this only rules
out immediate-constant sends; a `cmd` value computed into a register from elsewhere (a
variable, a lookup table) wouldn't show up this way, so this isn't a fully exhaustive proof
the way the `writeDatas()` sweep of `MCUAdapter_BoxP300` was. Not chased further -- flagged
here mainly so this real, substantial sibling library isn't rediscovered from scratch if a
future pass needs it (e.g. for tracing what these other vehicle-brand adapters do with
`CMD 0x84`/`0x87`, or anything else this doc has found dead-by-design on `BoxP300`).
