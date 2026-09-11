# Clean-Room STM32F105 IAP Bootloader

A clean-room reimplementation of a first-stage IAP (In-Application
Programming) bootloader for the STM32F105RBT6 companion MCU, linked to
run at `0x08000000`-`0x08003FFF` (below the application's own
`0x08004000` base). This is **not** an extraction or dump of the real
vendor bootloader that ships on actual hardware -- see the correction
notice below before trusting anything about its real-world accuracy.

---

## What it actually does (verified against its own current source)

`src/main.c` -- the whole real control flow:

1. On boot, check a fixed SRAM cell (`BOOTLOADER_MAGIC_ADDR`,
   `0x20004004`) for a magic cookie (`BOOTLOADER_MAGIC_VAL`,
   `0x5555AAAA`), then immediately clear it.
2. If the magic wasn't set and a valid-looking application image
   already exists at `0x08004000` (plausible stack pointer + reset
   vector, checked via `is_app_valid()`), jump straight there.
3. Otherwise, initialize USART2 at 38400 baud and run
   `ymodem_receive_and_flash()` (`src/ymodem.c`) -- a standard YMODEM
   receiver (`SOH`/`STX`/`EOT` framing, CRC16) that erases the
   application flash region and writes each received block via
   `flash_write_page()`.
4. On success, jump to the newly-flashed application. On failure,
   trigger a system reset via `AIRCR` and try again from the top.

**This is a write-only IAP receiver.** There is no memory-read,
flash-dump, or diagnostic-readback function anywhere in this source --
`ymodem.c` exposes exactly `flash_unlock()`/`flash_lock()`/
`flash_erase_app_pages()`/`flash_write_page()`/the receive loop, and
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
