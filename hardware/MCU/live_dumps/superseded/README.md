# Superseded Files — Do Not Flash

Everything in this directory is kept for **historical provenance only**.
None of it should be flashed to any board, spare or otherwise.

These are the original early-September 2026 extraction and patch attempts,
made before the Raspberry Pi Pico CMSIS-DAP probe transition and before the
disassembly-driven, hardware-verified reconstruction work. They are real
data (not fabricated) but are known to be less accurate, less complete, and
in some cases built on artifact words that were later root-caused and
corrected.

**The current, authoritative dumps and reconstructions live in
[`../vehicle_live_2026-09-14/`](../vehicle_live_2026-09-14/README.md).**
That directory's `README.md` documents the final, hardware-verified,
zero-fault working images — use those instead.

## What's here and why it's superseded

| File | Superseded by | Reason |
| :--- | :--- | :--- |
| `live_bootloader.bin` | `vehicle_live_2026-09-14/live_factory_bootloader_12k_raw.bin` | Earlier, less complete raw extraction pass (pre-Pico-probe transition). |
| `live_app_1302.bin` | `vehicle_live_2026-09-14/live_factory_app_52k_raw.bin` | Same — earlier, partial raw extraction. |
| `factory_bootloader_12k.bin` | `vehicle_live_2026-09-14/live_factory_bootloader_12k_raw.bin` | Realigned copy of the above; same underlying limitations. |
| `factory_bootloader_12k_patched.bin` | `vehicle_live_2026-09-14/live_factory_bootloader_12k_reconstructed.bin` | Only patched 2 known extractor-artifact words (SP/reset vector, one RCC_BASE literal) — not a full reconstruction, and does **not** include the later-found real bugs and fixes documented in `docs/BOOTLOADER_HANG_TRACE_2026-09-14.md`. |
| `factory_app_1302_52k.bin` | `vehicle_live_2026-09-14/live_factory_app_52k_raw.bin` | Earlier raw extraction. |
| `factory_app_1302_52k_patched.bin` | `vehicle_live_2026-09-14/live_factory_app_52k_reconstructed.bin` | Same 2-word-only patch limitation as above; superseded by the 100%-complete (573/573 word) reconstruction. |
| `factory_full_64k.bin` | `vehicle_live_2026-09-14/live_factory_bootloader_12k_raw.bin` + `live_factory_app_52k_raw.bin` | Combined dump from the earlier pass; both halves individually superseded. |
| `factory_full_64k_patched.bin` | `hardware/MCU/live_dumps/combined_factory_boot_factory_app_64k.bin` | Superseded by the final, hardware-verified, zero-fault combined image. |

If you're looking for something to actually flash, see the top-level
[`hardware/MCU/live_dumps/README.md`](../README.md) for the current
recommended images.
