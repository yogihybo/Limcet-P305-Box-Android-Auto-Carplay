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

The **real** vendor bootloader that ships on actual hardware at
`0x08000000`-`0x08003FFF` has never been captured by this project. The
one file that ever claimed to be a real dump of it
(`hardware/MCU/live_dumps/live_bootloader.bin`) was independently
proven fabricated and retracted -- see that directory's own README and
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s retraction section. Whether
the *real* bootloader's real IAP protocol implements any read-back or
diagnostic capability this clean-room reimplementation doesn't is a
genuinely open question -- RDP Level 1 (confirmed elsewhere in this
project to gate only the external SWD/JTAG debug port) would not
prevent such a capability from existing, if the real vendor code
happens to have one. This can only be resolved by a real hardware
experiment (trigger `CMD 0xE1` on actual hardware, then probe
`/dev/ttyHS0` for any response beyond standard YMODEM handshake
bytes), not by anything checkable in source.
