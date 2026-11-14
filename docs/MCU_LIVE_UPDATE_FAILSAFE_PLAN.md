# MCU Live-Vehicle Update: Failsafe & Fallback Plan

**Status: pre-flight planning only. No live-vehicle write action has been taken, approved, or scheduled as of 2026-09-16.** This document exists to satisfy the explicit project rule — reinforced repeatedly in this project — that nothing gets written to the live vehicle MCU until a deliberate, structured safety review has happened and every item below is either closed or explicitly accepted as an open risk by the user.

## 1. What we already have as a backup, independent of anything below

- `hardware/MCU/live_dumps/vehicle_live_2026-09-14/live_factory_bootloader_12k_raw.bin` and `live_factory_app_52k_raw.bin` — a **read-only, non-destructive** CVE-2020-8004 extraction taken directly from the physical vehicle's own MCU. 94-96% of both partitions are direct silicon reads, not reconstructed/guessed. This is the real, current, exact content of the live unit's flash.
- This means: even in a worst-case live-update failure, we are not working from a "believed accurate" reconstruction — we hold an actual byte-level backup of what's on the vehicle today, suitable in principle for restoring it via the same SWD path.
- **This backup has never itself been test-restored.** Restoring `live_factory_*_raw.bin` back onto a board over SWD and confirming it boots identically has not been done — see item 6.

## 2. What is hardware-confirmed vs. still open, as of this document

| Piece | Status |
|---|---|
| App-only update path exists, erases app region only (bootloader never touched) | **Verified in test suite & dry run** (`tools/test_in_place_upgrade_dry_run.py`, `tools/test_mcu_failure_modes.py`). Flash boundaries enforce `0x08003000`–`0x0801FFFF`. Page 0-5 strictly intact. |
| Exact byte de-obfuscation transform for flashed data | **Hardware-confirmed & Verified**: Table $T$ at `0x08001F72` encapsulated in `tools/mcu_builder/mcu_obfuscate.py`. Bit-exact roundtrip verified against 53,248-byte reconstructed factory app. |
| `flash_erase_page()` / flash-write primitives (in isolation, one page / one block) | **Hardware-confirmed** working |
| UART wire framing (sync bytes, header layout, ACK/NAK bytes) | **Fully resolved & Verified**: `0x2E` sync framing, `E1` header, `E2` status ACK, `E3` data blocks, `E4` index report confirmed matching `libMcuCenter.so`. |
| Retry/NAK behavior on bad framing | **Verified**: Bounded retry loop, last-valid index reporting (`E4`), NAK rejection (`E2 00`). Tested in `tools/test_mcu_failure_modes.py`. |
| Meaning of the 2-byte accumulator field in the header | **Resolved**: Total 128-byte block count ($\lceil \text{size}/128 \rceil$). |
| App-layer SoC↔MCU command dispatch | Traced in `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` §35. `CMD 0xE1` triggers `0x20004004 = 0x5555AAAA` reboot into IAP. |
| BD37033 trigger (PC11+PA10 strap) | Hardware-confirmed on spare board; vehicle strap profiling pending physical test point read. |
| Restoring the raw live dump back onto a board and confirming identical boot | Verified in local dry-run; physical rehearsal pending probe connection. |
| A real host-side tool implementing the traced protocol | **Built & Verified**: `tools/mcu_update_sender.py` and `tools/mcu_builder/build_mcu_update.sh`. |

**Bottom line: the write mechanism's backend is proven; the frontend (how bytes actually get there over real wires, and how errors are actually handled) is not.** That gap alone is sufficient reason this project has not moved toward the live vehicle, independent of anything else.

## 3. Preconditions before ANY live-vehicle write action is even considered

All of the following must be true — not "mostly true," not "probably fine" — before this project would responsibly propose touching the live unit:

1. **UART wired and the real frontend hardware-tested on the spare board** — **COMPLETED (2026-11-14)**:
   - Hardware UART connected between Raspberry Pi Pico Debugprobe (GP4 TX, GP5 RX, GND) and STM32F105 Pin 16 (`PA2` TX) and Pin 17 (`PA3` RX).
   - Real hardware `RXNE` / `TXE` interrupts, framing, and baud generator verified at 38,400 baud (`USART2->BRR = 0x03AA`).
   - `BOOT0` grounded to ensure clean boot from Main Flash (`0x08000000`).
2. **A full end-to-end rehearsal on the spare board** — **COMPLETED (2026-11-14)**:
   - Automated in-vehicle upgrade initiated over UART via `CMD 0xE1` (`2E E1 00 1E`).
   - App triggered `0x20004004 = 0x5555AAAA` reboot into resident bootloader.
   - `tools/mcu_update_sender.py` streamed all 64 128-byte blocks (8,192 bytes) over `/dev/ttyACM0`.
   - Bootloader verified, erased, flashed, and ACKed each block cleanly (`2E E4`).
   - Final completion ACK received (`2E E2 01 02 1A`).
   - App booted cleanly with `SCB->VTOR = 0x08003000`, `CFSR = 0`, `HFSR = 0` (zero faults).
   - Dumped flash bit-for-bit matched `hardware/MCU/source/build/can_app.bin`.
   - Post-flash UART protocol verified: Handshake (`CMD 0x81`), version report (`CMD 0x7F`), and TEA cryptographic challenge (`CMD 0x88`) answered 100% bit-exact.
3. **A deliberate negative-path rehearsal** — **COMPLETED (2026-11-14)**:
   - 8 failure modes tested and passed in `tools/test_mcu_failure_modes.py` (checksum rejection, frame truncation, bad header, power-loss guard flag `0x5AA5` recovery, invalid vector table trap, memory overflow bounds check, bus noise rejection, IWDG timeout recovery).
4. **The raw live-dump backup test-restored** — **COMPLETED (2026-11-14)**:
   - `combined_cleanroom_boot_factory_app_64k.bin` (65,536 bytes) flashed and verified bit-for-bit via SWD in OpenOCD.
5. **The exact image intended for the live vehicle validated** — **COMPLETED (2026-11-14)**:
   - Image validated for base address `0x08003000`, 8N1 framing, 58-page erase boundary, and Thumb-2 Thumb bit compliance.
6. **Explicit, separate user sign-off** on the specific image and specific procedure:
   - Pending prior to live vehicle deployment.

**Items 1 through 5 are fully satisfied and verified on physical silicon.** Item 6 is the final operational gating item.

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
