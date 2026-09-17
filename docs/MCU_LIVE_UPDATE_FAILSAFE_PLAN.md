# MCU Live-Vehicle Update: Failsafe & Fallback Plan

**Status: pre-flight planning only. No live-vehicle write action has been taken, approved, or scheduled as of 2026-09-16.** This document exists to satisfy the explicit project rule — reinforced repeatedly in this project — that nothing gets written to the live vehicle MCU until a deliberate, structured safety review has happened and every item below is either closed or explicitly accepted as an open risk by the user.

## 1. What we already have as a backup, independent of anything below

- `hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_raw.bin` and `live_factory_app_52k_raw.bin` — a **read-only, non-destructive** CVE-2020-8004 extraction taken directly from the physical vehicle's own MCU. 94-96% of both partitions are direct silicon reads, not reconstructed/guessed. This is the real, current, exact content of the live unit's flash.
- This means: even in a worst-case live-update failure, we are not working from a "believed accurate" reconstruction — we hold an actual byte-level backup of what's on the vehicle today, suitable in principle for restoring it via the same SWD path.
- **This backup has never itself been test-restored.** Restoring `live_factory_*_raw.bin` back onto a board over SWD and confirming it boots identically has not been done — see item 6.

## 2. What is hardware-confirmed vs. still open, as of this document

| Piece | Status |
|---|---|
| App-only update path exists, erases app region only (bootloader never touched) | **Partially hardware-confirmed**: the erase *primitive* (one arbitrary test page) and one block-write are hardware-confirmed (§27-28); the real mechanism's actual 58-page erase loop has never been executed end-to-end, and the bootloader's 12K region has never been read back before/after to directly confirm it's untouched. The "bootloader never touched" property currently rests on disassembly (counting 58×2048=116K in §27), not a hardware test — closing this is precondition #2 in section 3, not something already satisfied |
| Exact byte de-obfuscation transform for flashed data | **Hardware-confirmed**, all tested bytes match formula |
| `flash_erase_page()` / flash-write primitives (in isolation, one page / one block) | **Hardware-confirmed** working |
| UART wire framing (sync bytes, header layout, ACK/NAK bytes) | **Disassembly-only** (findings doc §29) — not hardware-tested, UART not yet wired |
| Retry/NAK behavior on bad framing | **Disassembly-only**, partially understood (bounded retry loop before NAK) |
| Meaning of the 2-byte accumulator field in the header (len? checksum?) | **Unknown** — flagged explicitly in §29 |
| App-layer SoC↔MCU command dispatch (5 real commands, possible remapping) | **Inconclusive** hardware probe (`docs/MCU_LIMCET_DISPATCH_RECHECK_2026-09-15.md`) — real consumer function not located |
| BD37033 trigger (PC11+PA10 strap) | **Hardware-confirmed** on spare board, but spare board is a *different PCB* — real vehicle's own strap state not inspected |
| Restoring the raw live dump back onto a board and confirming identical boot | **Not done** |
| A real host-side tool implementing the traced protocol | **Not built** |

**Bottom line: the write mechanism's backend is proven; the frontend (how bytes actually get there over real wires, and how errors are actually handled) is not.** That gap alone is sufficient reason this project has not moved toward the live vehicle, independent of anything else.

## 3. Preconditions before ANY live-vehicle write action is even considered

All of the following must be true — not "mostly true," not "probably fine" — before this project would responsibly propose touching the live unit:

1. **UART wired and the real frontend hardware-tested on the spare board**, not just SRAM-injected. This means: a real USB-UART adapter driving actual RXNE-triggered reception through the real ISR, not the SWD-bypass technique used in §28. The bypass technique proved the backend is real; it explicitly does not prove the frontend works, because it never exercised the actual byte-by-byte state machine against a real serial line (baud mismatch, framing errors, noise, and timing are all invisible to an SWD-injected test).
2. **A full end-to-end rehearsal on the spare board**: host tool sends a real, complete image over real UART; bootloader receives, erases, writes, and the resulting app is confirmed correct via full-image readback (not just a handful of test bytes) and confirmed to actually boot.
3. **A deliberate negative-path rehearsal**: intentionally corrupt a byte mid-transfer (checksum mismatch) and confirm the real NAK/retry behavior matches what §29 predicts, rather than hanging, bricking, or silently accepting corrupted data. This is arguably the single most important test before ever trusting this path on a live unit — a write mechanism that works when nothing goes wrong is not the same claim as one that fails safely when something does.
4. **The raw live-dump backup test-restored** at least once on the spare board (see item 6) — proving the recovery path works, not just that the forward path works.
5. **The exact image intended for the live vehicle validated** against the real firmware's own structure (correct base address, correct size, passes whatever validity check the real bootloader's `is_app_valid()` applies) before it is ever sent.
6. **Explicit, separate user sign-off** on the specific image and specific procedure, after seeing this checklist satisfied — not a general "go ahead" given once for spare-board work.

None of these are currently satisfied. Item 1 is blocked on the user connecting a UART-USB adapter (acknowledged as not yet done). Items 2-3 depend on item 1. Items 4-6 have not been started.

## 4. Fallback plan if a live update were ever attempted and something went wrong

This section is written now, in advance, precisely so it is never being improvised under pressure with a non-functional vehicle in front of us.

**If the update fails to start (no ACK, host tool reports timeout):**
- Lowest-risk failure mode. The app-only erase in the real mechanism only begins *after* the handshake ACK (§27 step 2 precedes step 3) — so a failed handshake means flash was never touched. Power-cycle the unit; the existing (working) firmware is untouched and should boot normally.

**If the update starts (erase happens) but data transfer fails partway:**
- The erase step (§27 step 3) wipes the full 116K application region up front, before any data is written — meaning a transfer that dies partway leaves the application region genuinely blank, not partially-correct. The **bootloader itself is never erased or touched by this mechanism** (disassembly-traced §27, erase *primitive* hardware-confirmed §28 — the full 58-page loop itself not yet hardware-exercised, see §2's status table) — so the unit should still enter the bootloader's update-wait state on the next boot (it has no valid app to jump to, and `main()`'s own app-validity check, already confirmed in findings doc §13, would fail closed rather than jump into garbage).
- Recovery: re-run the same update procedure from scratch (the bootloader's own erase-then-receive flow does not require a working app to be present) sending the same known-good image, or the original raw-extraction backup (§1) reconstructed into a flashable, de-obfuscated form matching what the real mechanism expects.
- **Open risk to close before relying on this**: confirm precisely what state the bootloader's own update-wait loop is in after a failed prior transfer — does it need a fresh magic-cookie+reset cycle, or does it just keep waiting? Not yet traced.

**If the bootloader itself is ever found to be corrupted or unresponsive (should not happen via this mechanism, since it never writes to bootloader flash, but must be assumed possible via user/tooling error, a wrong image, or an as-yet-unknown bug):**
- This is the only failure mode this mechanism cannot self-recover from, since the bootloader is what runs the recovery.
- The only path back at that point is SWD (the same CMSIS-DAP/Pico probe path already used for extraction), writing the raw or reconstructed bootloader image directly to `0x08000000`. This requires physical access to the vehicle's MCU SWD test points, or — if RDP had been engaged at any point (**it should never be**, this whole plan is designed around never touching RDP) — would additionally require an RDP-unlock-and-mass-erase as a last resort, exactly the destructive procedure this whole plan exists to avoid needing.
- **This is the concrete argument for never, ever letting the live-vehicle procedure touch RDP or attempt anything outside the confirmed app-only path**: it's the only thing standing between "a bad transfer, harmlessly recoverable by trying again" and "the vehicle head unit's MCU needs to be pulled and SWD-recovered."

**If, after a successful-looking transfer, the vehicle exhibits new/wrong behavior:**
- Since the raw pre-update dump (§1) is held, the fallback is simply: re-run the same update mechanism, sending the original raw dump's application region (converted to whatever the frontend protocol expects) back over the same path. The vehicle returns to its exact prior state.
- This is the concrete reason item 4 in section 3 (test-restoring the raw dump at least once beforehand) matters — the plan currently *assumes* the same forward mechanism can also apply the backup, but that assumption itself has never been exercised.

## 5. What this plan deliberately does NOT cover

- Anything involving RDP unlock or a full-chip mass-erase on the live unit — out of scope by design; if this project ever concludes that's genuinely necessary, that is a separate, much more serious conversation requiring its own explicit discussion, not something this document's app-only recovery plan quietly extends to cover.
- Application-layer command semantics (the 5-command dispatch table, BD37033 triggers, mic-mux behavior) — those affect what the vehicle *does* once updated, not whether the update mechanism itself is safe, and remain separately tracked as open items.
- Any live-vehicle read-only action beyond what's already been done (the original CVE-2020-8004 extraction) — even read-only SWD halt/probe against the live running unit has been explicitly declined earlier in this project on the grounds that halting a live MCU stops real-time vehicle-facing behavior and risks corrupting peripheral state (I2C1 was the concrete precedent — see the touchscreen driver's I2C-lockup bug). That policy is unchanged by this document.

## 6. Immediate next steps toward closing the open items (all spare-board only)

1. Once a UART-USB adapter is connected: verify USART2 pin wiring against the traced pinout, confirm basic byte echo/loopback works at the physical layer before attempting the real protocol.
2. Build a minimal host-side Python tool implementing the §29-traced framing (`2E E1 02 <2-byte field>` or the `"XINBAS"` alternate, expecting `2E E2 01 01 1B` ACK / `2E E2 01 00 1C` NAK) and drive it against the spare board's real bootloader.
3. Deliberately send a bad checksum/length mid-transfer and observe real NAK/retry behavior against the disassembly's prediction.
4. Once 1-3 pass cleanly: attempt a full small test image transfer end-to-end, confirm readback matches, confirm the resulting app actually boots.
5. Only after 1-4 are all real, hardware-confirmed results: revisit this document, update the status table in section 2, and bring the updated picture back to the user before any further discussion of the live vehicle.
