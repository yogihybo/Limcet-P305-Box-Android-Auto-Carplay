# Clean-Room STM32F105 IAP Bootloader

A clean-room reimplementation of a first-stage IAP (In-Application
Programming) bootloader for the STM32F105RBT6 companion MCU, linked to
run at `0x08000000`-`0x080027FF` (below Page 5's `0x08002800` flag page and
the application's own `0x08003000` base).

---

## What it does (Section 36 Overhaul — Drop-in Factory Compatible)

`src/main.c` and `src/update_protocol.c` implement the exact, authentic OEM
update protocol and safety mechanisms matching the factory bootloader:

1. **Clock Configuration**: 72 MHz clock tree (`CFGR2 = 0x00010644`, PLL2 *8,
   PREDIV1 /5, PLL *9), with bounded HSE wait (~0x1000 polls) falling back to
   8 MHz HSI on crystal failure.
2. **Boot Checks**:
   - Checks if update requested via SRAM magic (`*0x20004004 == 0x5555AAAA`) OR
     interrupted-update flag in flash (`*(uint16_t *)0x08002FFE == 0x5AA5`).
   - If neither and application initial stack pointer passes masked check
     (`(SP & 0x2FFE0000) == 0x20000000` at `0x08003000`), performs settling delay,
     writes authentication handshake (`*0x20004000 = 0x20141003`), relocates VTOR
     to `0x08003000`, sets MSP, and jumps to application `Reset_Handler`.
3. **OEM IAP Update Mode**:
   - Configures power rails: asserts `PC14=1`, `PB4=1`, `PC12=0` to keep ARK1680 SoC
     rails powered during flashing.
   - Starts hardware IWDG watchdog (prescaler /128, reload `0x270`, ~2.0s timeout).
   - Initializes USART2 at 38400 8N1 on PA2 (TX) / PA3 (RX).
   - Listens for head unit's `CMD 0xE1` header (`2E E1 02 <total_blocks_hi> <total_blocks_lo> <chk>`)
     or `2E E1 08 ... "XINBAS" <chk>`.
   - Replies with `2E E2 01 01 1B` (Ready ACK).
4. **Flashing & De-obfuscation**:
   - On the first block: erases the 58 application pages (`0x08003000`–`0x0801FFFF`)
     and programs `0x5AA5` to `0x08002FFE` (interrupted-update guard).
   - Receives each 128-byte block (`2E E3 82 <block_hi> <block_lo> <data[128]> <chk>`).
   - De-obfuscates payload using the 128-byte table `T` at `0x08001F72`:
     `flash[i] = (~wire[i] + T[i % 128]) & 0xFF`.
   - Programs de-obfuscated halfwords into flash and replies with `CMD 0xE4` status.
5. **Completion**:
   - When all blocks are written: erases Page 5 (`0x08002800`) to clear the `0x5AA5` flag,
     sends `2E E2 01 02 1A` (done), clears `0x20004004 = 0`, locks flash, and spins until
     IWDG resets the MCU into normal boot.

6. **Recovery when the host goes quiet** (timings measured on the bench against the
   factory bootloader; `MCU_FIRMWARE_VERIFIED_FINDINGS.md` section 38):
   - Waiting for the header: sends `2E E2 01 00 1C` every 7.0 s; after 358 s it gives up,
     clears `0x20004004` and lets the IWDG reset the MCU, so the application runs again.
   - During the transfer: after 19.6 s with no bytes it drops any partial frame and
     re-requests -- `E2 01` before the first block (host restarts at block 0), `E4 <last>`
     after it (host resends the next block). With no complete frame for 314 s it gives up
     the same way; the `0x5AA5` flag then brings it straight back to update mode.
   - A corrupt frame gets no reply (the re-request recovers); a good frame for the wrong
     block is answered `E4 <last>`. Headers with a block count of 0 or more than 928
     (116 KB) are answered `E2 00` and the wait continues.
   - The baud rate is derived from the clock actually running, so the 8 MHz HSI fallback
     still talks at 38,400.

**Not reproduced from the factory:** if the application pages are write-protected, the
factory erases the option bytes (keeping the readout-protection level) before the
application erase. That path is deliberately left out -- getting it wrong on an RDP1 part
mass-erases the whole chip, bootloader included -- so a write-protected part simply fails
to update.

**This is a write-only IAP receiver.** There is no memory-read,
flash-dump, or diagnostic-readback function anywhere in this source --
`update_protocol.c` exposes exactly `flash_unlock()`/`flash_lock()`/
`flash_erase_app_pages()`/`update_process_packet()`/the receive loop, and
nothing else. Structurally, this matches the real, disassembly-
confirmed one-way push flow this project traced on the SoC side
(`libMcuCenter.so`'s `sendYModemDatas()`/`onSendUpdateReadyTimer()` --
see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s `CMD 0xE1` section):
Linux only ever sends firmware blocks, never asks for anything back.

The `0x20004004`/`0x5555AAAA` magic pair here is a real match for what
this project's own disassembly of `can_app.bin`'s `CMD 0xE1` handler
found (same section of `MCU_FIRMWARE_VERIFIED_FINDINGS.md`) -- this
bootloader was written to interoperate with that real, confirmed
mechanism, not guessed independently of it.

---

## Correction notice: this bootloader does NOT implement a "diagnostic memory read command (0x90)"

The commit that introduced this directory
(`02b46048`/`3921b909` -- same content, duplicated across this
project's two-checkout history) is titled *"add cleanroom STM32F105
IAP bootloader and diagnostic memory read command (0x90)"*. **That
title does not match what the commit actually contains.** Its real
diff touches only files under `hardware/MCU/bootloader/`, and neither
that diff nor this directory's current source contains any `0x90`,
`DIAG`, `diagnostic`, or `READ_MEM` reference anywhere -- confirmed by
direct grep, not assumption.

The likely explanation: the same session that authored this commit
also touched the *application* source's `SOC_CMD_DIAG_READ_MEM`
(`0x90`) definition in `hardware/MCU/source/`, and the commit message
here is a stale/copy-pasted description of that unrelated, separate
change rather than this one. That app-side `CMD 0x90` was later
checked directly against this device's real `can_app.bin` and 4 other
real reference firmware images this project holds -- confirmed absent
from all 5 -- and removed from the clean-room application source as a
proven fabrication (`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`, "CMD
0x90 -- disproven" section).

This bootloader was re-audited on 2026-08-30 specifically to check
whether it might independently carry the same or a similar capability
under a different name. It does not. The commit message is simply
wrong about what this directory contains, and is left uncorrected at
the git-history level (rewriting an already-pushed, already-merged
commit message would require an interactive rebase, which is not
available in this environment, and would be a disruptive rewrite of
shared history regardless) -- this README is the durable correction
going forward.

---

## What remains genuinely unknown

**Correction (2026-09-12): this section is stale.** It previously
stated the real vendor bootloader "has never been captured by this
project" and that `hardware/MCU/live_dumps/live_bootloader.bin` "was
independently proven fabricated and retracted." That was true of the
August 28 extraction attempt (a real, documented failure of the
original `tools/stm32f1_extractor_fixed.py` script), but a September
11-12, 2026 re-extraction using a Raspberry Pi Pico CMSIS-DAP probe
(replacing the earlier ST-Link HLA adapter) produced a dump that has
since been independently authenticated: reproducible across multiple
runs, cross-referenced against real literal-pool data in the
application firmware, with the one confirmed artifact word
root-caused to a real Cortex-M `INVSTATE` fault rather than
fabrication. See `hardware/MCU/live_dumps/README.md` and
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` sections 5-6 for the full
verification methodology and scope limits (not every byte has been
independently cross-referenced).

This clean-room bootloader was written before that real dump existed,
from generic STM32F1 reference behavior and the unrelated Volvo
`DCn32` firmware package -- it should not be treated as a spec for the
real vendor bootloader's behavior. Disassembly of the real dump has
already surfaced concrete, confirmed divergences (application base
address, the `flash_unlock()` LOCK-bit guard) that have been corrected
in this directory's source; others remain open. See the MCU bootloader
bring-up plan and `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` for current
status before assuming this clean-room implementation matches real
hardware behavior in any area not explicitly marked as disassembly-verified.

Whether the *real* bootloader's IAP protocol implements any read-back
or diagnostic capability this clean-room reimplementation doesn't is
still a genuinely open question, not yet resolved by the disassembly
work done so far -- worth revisiting now that a real dump exists to
check against, rather than only via a live hardware experiment.
