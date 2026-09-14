# Re-checking the SoC↔MCU command dispatch against the real Limcet firmware (2026-09-15)

## Context and why this doc exists

Every finding in `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` and
`docs/MCU_COMMAND_REFERENCE.md`'s MCU-side sections was derived from
`hardware/MCU/can_app.bin` — a **generic `DCn32-VOLVO-V2.10-20240909`
reference build**, not this project's real vehicle firmware (see both
docs' scope-clarification sections, updated 2026-09-15 to reflect this
doc's existence).

As of 2026-09-14, this project holds a real, 100%-word-reconstructed,
hardware-verified dump of the **actual** firmware running on this
vehicle's MCU:
`hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin`
(SHA-256 `3ec2715d13bbcb0fd924d2b517eae32b98e83395611526a5397eb2f5c3be6960`),
whose own embedded version string reads `Limcet-V1.0-1302` — a
genuinely different build from the Volvo reference, confirmed by direct
byte comparison (after correcting for the two files' different base
addresses, the majority of bytes differ).

This doc re-derives the SoC→MCU command dispatch directly from the real
Limcet binary and compares it against the Volvo-derived findings.
**Method**: direct Capstone (ARM Thumb) disassembly plus raw
little-endian word scanning of the binary on disk, following this
project's own established practice of anchoring every claim to control-flow-
or data-structure-verified evidence, not blind linear disassembly sweeps
(this project has repeatedly hit and documented literal-pool/code
interleaving problems with naive linear sweeps — see below for a live
example hit and caught during this exact pass).

## Headline finding: the real Limcet MCU firmware has only 5 SoC→MCU command handlers, not 9

The Volvo reference build's SoC→MCU dispatch table has 9 entries:
`0x81, 0x82, 0x84, 0x85, 0x87, 0x88, 0xA0, 0xE1, 0xFF` (per
`docs/MCU_COMMAND_REFERENCE.md`'s own already-confirmed table).

The real Limcet firmware's equivalent table was located directly (raw
word scan, `(handler_ptr, cmd_byte)` 8-byte-stride pairs, at file/flash
address `0x0800B988`):

| Wire cmd byte | Handler address |
|---|---|
| `0x81` | `0x08006352` |
| `0x82` | `0x0800643C` |
| `0xA0` | `0x080065D8` |
| `0xFF` | `0x08006468` |
| `0xE1` | `0x0800660E` |

**Only 5 entries.** An exhaustive raw-word scan of the entire 53,248-byte
binary for any `(handler_ptr, cmd_byte)` pair with `cmd_byte` equal to
`0x84`, `0x85`, `0x87`, or `0x88` found **zero** matches anywhere — not
just absent from this specific table, absent from the whole binary as a
dispatch entry.

**This is not surprising in isolation — it's independently corroborated
by this project's own prior SoC-side research.** `docs/MCU_COMMAND_REFERENCE.md`'s
"Full stock-software `CMD` flow map" section (2026-09-02) already
exhaustively traced every real transmit call site inside
`MCUAdapter_BoxP300` — the specific vehicle-adapter class confirmed
active on this exact product — and found it **never sends `0x84`,
`0x85`, `0x87`, or `0xFF`, ever.** (`0x88` is sent, but by `MsnCoreApp`
directly, bypassing the adapter class entirely.) Two independent
binaries — the SoC's own application software and now the MCU's own
real firmware — agree: `0x84`/`0x85`/`0x87` are dead on this product on
both ends of the wire. That's strong, mutually-corroborating evidence,
not a coincidence.

## This resolves a previously open mystery in this project's own docs

`docs/MCU_COMMAND_REFERENCE.md` (§ on `CMD 0x88`, 2026-09-04) recorded a
real, unresolved question: on real hardware, `custom_ui`'s periodic
`CMD 0x88` probe (TEA-cipher challenge) never got a single reply, despite
both candidate MCU firmwares (the Volvo reference disassembly, and this
project's own clean-room `hardware/MCU/source/`) appearing to implement
a working `0x88` handler. The doc's own words: *"why neither candidate
MCU firmware's own documented/coded `CMD 0x88` handling actually
produces a reply on this real hardware is still unresolved... does its
`SOC_CMD_CRYPTO_CHALLENGE` dispatch entry actually get reached in
practice?"*

**Now answered, with real evidence**: it doesn't get reached, because
the real, actually-running Limcet firmware genuinely has no `0x88`
dispatch entry at all. Not a mystery, not a timing issue, not a link
problem — the real vehicle's MCU firmware was never built to answer that
command in the first place. (Marked resolved in
`docs/MCU_COMMAND_REFERENCE.md`.)

## A more significant, NOT yet fully confirmed lead: command-byte semantics may not match Volvo's 1:1

Having found only 5 real entries, the next question was whether each
surviving command byte does the *same thing* the Volvo reference's
same-numbered command does. Disassembling each of the 5 handler bodies
(each function is a real, CPU-verified call target — it's the literal
value stored in the table itself, so its start address is guaranteed to
be a valid instruction boundary, sidestepping most of the linear-sweep
alignment risk) gave a clean, self-consistent picture: each function
ends in `pop {r4,pc}` (or `bx lr`) landing exactly at the next table
entry's own address, with no gaps or garbled decode in between —
different from a specific alignment problem hit and caught nearby (see
"a genuine near-miss," below).

**Observed handler bodies**:

| Wire cmd (Limcet) | Behavior found | Best structural match to a Volvo command |
|---|---|---|
| `0x81` | `nop; bx lr` — a true, literal no-op | — (Volvo's `0x81` broadcasts version/status; this build's `0x81` does nothing) |
| `0x82` | Calls one internal queue function 4 times with different "type" arguments (`0xB, 1, 7, 9`), each with a fixed `0` payload | Doesn't match Volvo's `0x82` (a single 2-byte state-struct write); more like a startup/init broadcast burst |
| `0xA0` | Reads `payload[2]`; on `==1` writes a 2-byte struct `{1,4}` then one queue call; else writes `{2,1}` then the same queue call | **Matches Volvo's `CMD 0x82` (app-state) exactly** — same `{1,4}`/`{2,1}` struct-write shape, same single queue call |
| `0xFF` | `cmp r0,#0x11` (17) bound check, `tbb` 17-entry jump table on a setting id read from `payload[2]` | **Matches Volvo's `CMD 0xA0` (settings sync) shape** — and its own id=`0x00`/id=`0x09` sub-cases write to struct offsets `0x3b`/`0x38`, the **exact same offsets** this project's own Volvo-derived research independently named `mode_3b` and `mic_mux_38` |
| `0xE1` | Reads `payload[2]` as a sub-id, checks against `0, 6, 7, 8, 9, 0x7F`; only `0x7F` calls the queue function (`type=0xB`), everything else is a no-op | **Matches Volvo's `CMD 0xFF` (system-reset/sub-id dispatch)** shape closely |

If this reading is correct, the wire-command **numbers** for
"app-state," "settings-sync," and "sub-id dispatch" have shifted
relative to the Volvo reference build — while the underlying
*mechanisms themselves* (the same struct offsets, the same queue
function, the same debounce/dispatch shapes) are unchanged. That's
consistent with this being the same `DCn32` codebase compiled with a
different **wire protocol table** for this product, not a different
codebase.

**Why this matters, concretely**: this project's own clean-room
`hardware/MCU/source/include/uart_protocol.h` hardcodes
`SOC_CMD_SYNC_SETTINGS = 0xA0` (matching Volvo). If the real vehicle's
actual MCU firmware — and, matching it, the real SoC application
software's actual send-side byte choices — use `0xA0` for something
else and reserve a **different** byte for settings-sync, then
`custom_ui`/the real SoC software's UI-settings commands (mic-mux,
camera relay toggles, etc.) would land on the clean-room's `handle_app_state()`
path instead of `handle_sync_settings()`, or vice versa, if this
project's own clean-room firmware were ever flashed to a real vehicle.

**This is NOT yet fully confirmed and should not be treated as settled.**
Two real gaps remain:

1. **The handler-body reading above, while self-consistent and
   boundary-verified, was NOT cross-checked against a full
   recursive-descent trace from `Reset_Handler`** (this project's own
   proven, most-reliable disassembly methodology for exactly this class
   of ambiguity — see `docs/BOOTLOADER_HANG_TRACE_2026-09-14.md` sections
   15/22 for where it mattered before). A genuine near-miss was hit and
   caught during this pass: address `0x08006358` (immediately after wire
   command `0x81`'s true `nop; bx lr` body) was initially misread by a
   blind linear disassembly as real Thumb instructions (`cbnz`, etc.);
   directly checking the raw bytes showed it's actually the start of the
   next data structure's literal pool (`0x0800B988`, coincidentally the
   dispatch table's own base address, sitting nearby as inline data), not
   code at all. The 5 handler-body traces above were re-verified to be
   clean of this specific failure mode (self-consistent function
   boundaries, correct `push`/`pop` framing, landing exactly on the next
   table-verified address) — but a full recursive-descent confirmation
   was not completed given the depth already reached, and would be the
   right way to fully close this out.
2. **The real trigger path for genuine bootloader entry was traced to a
   concrete, real mechanism, but not conclusively tied to a specific
   wire command byte.** A real function at `0x0800B842` performs exactly
   the write this project's own clean-room bootloader assumes triggers
   an update (`*0x20004004 = 0x5555AAAA`, confirmed via the actual raw
   bytes at that literal-pool address), sitting in the same small literal
   pool as the previously-independently-found factory bootloader
   authentication handshake value (`*0x20004000 = 0x20141003`, matching
   the already-documented OEM handshake write at bootloader address
   `0x08001844`-`0x08001848`). This function is called
   from a tiny 3-instruction stub (`0x0800665C`: `push {r4,lr}; bl
   0x0800B842; pop {r4,pc}`), but that stub's own caller could not be
   located via direct `bl`/literal-pool cross-referencing within this
   pass's time budget — its only found reference is a single entry in a
   second, unexplained table (`0x0800B9B0`) whose own loading mechanism
   wasn't found either. **What this does establish, with reasonable
   confidence**: wire byte `0xE1` in the real Limcet firmware does the
   sub-id/`0x7F`-queue dispatch (structurally matching Volvo's `CMD
   0xFF`), **not** a direct bootloader-entry trigger the way Volvo's own
   `CMD 0xE1` does — meaning the real, safety-relevant assumption in
   `tools/mcu-probe/mcu-probe.c` (sending byte `0xE1` triggers bootloader
   mode) may not hold for the real vehicle firmware, but the *actual*
   trigger command was not conclusively identified in this pass. Left
   deliberately unresolved rather than guessed.

## Recommended next steps (not completed this pass)

1. **Recursive-descent verification** of the 5 handler-body traces above,
   from `Reset_Handler` (`0x08003599`, already confirmed real from the
   bootloader work), to fully rule out any remaining literal-pool/code
   interleaving risk in this specific region.
2. **Hardware verification, the gold standard this project has
   repeatedly favored over static analysis alone**: flash
   `live_factory_app_52k_reconstructed.bin` (paired with either the
   clean-room or factory bootloader) onto the spare test board, locate
   the real `g_rx_ring`-equivalent SRAM structure via disassembly (the
   literal-pool addresses referenced by the USART2 ISR), and directly
   inject each of the 5 wire command bytes via SWD — exactly the
   technique `tools/test_mcu_uart_protocol.py` already uses for the
   clean-room build — observing real GPIO/struct state changes to settle
   the command-byte-semantics question empirically rather than by
   disassembly alone. **Not done this pass** — this doc's findings are
   static-analysis-only and should be treated as a strong, well-evidenced
   lead, not a hardware-confirmed fact, until this step is done.
3. **Locate the real bootloader-entry trigger's actual caller/command
   byte**, since this directly affects the accuracy (not the
   conservatism — the existing warning remains the safe default either
   way) of `tools/mcu-probe/mcu-probe.c`'s own safety commentary about
   `CMD 0xE1`.

## What IS safe to update now, on this evidence

- `docs/MCU_COMMAND_REFERENCE.md`'s open `CMD 0x88`-never-replies
  mystery: resolved, real Limcet firmware has no `0x88` handler at all
  (independently corroborated by the SoC-side send-path research already
  in that doc).
- Both `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` and
  `hardware/MCU/MCU_FIRMWARE_REVIEW.md`'s scope-clarification sections:
  updated to point here and to the real dump, so future readers don't
  re-derive "has anyone checked this against the real vehicle firmware"
  from scratch.
- `tools/mcu-probe/mcu-probe.c`'s `CMD 0xE1` safety commentary: updated
  to note a real dump now exists, while keeping the existing conservative
  warning (never assume a "backup exists" makes flashing the live
  vehicle acceptable).
