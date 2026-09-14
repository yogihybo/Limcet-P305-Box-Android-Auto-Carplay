# Live STM32F105 MCU Firmware Dumps

> **Current, authoritative extraction and reconstruction work lives in
> [`vehicle_live_2026-09-14/`](vehicle_live_2026-09-14/README.md).** That
> directory's README documents the final, hardware-verified, zero-fault
> working images, including the flagship
> [`combined_factory_boot_factory_app_64k.bin`](combined_factory_boot_factory_app_64k.bin)
> — the fully-authentic reconstructed factory bootloader + 100%-reconstructed
> factory application, confirmed running end-to-end on physical silicon.
>
> Everything from the earlier (pre-2026-09-14) extraction pass has been
> moved to [`superseded/`](superseded/README.md). Those files are kept for
> historical provenance only — **do not flash anything from `superseded/`**.
> They predate the Pico CMSIS-DAP probe transition and the disassembly-driven
> bug-fixing work described in `docs/BOOTLOADER_HANG_TRACE_2026-09-14.md`,
> and in some cases are built on extraction-artifact words that were later
> root-caused and corrected.

---

## Directory layout

- **[`vehicle_live_2026-09-14/`](vehicle_live_2026-09-14/README.md)** —
  current raw silicon captures + fully-reconstructed bootloader and
  application, with full provenance and checksums.
- **[`combined_factory_boot_factory_app_64k.bin`](combined_factory_boot_factory_app_64k.bin)**
  — the flagship, ready-to-flash (spare test board only) combined image.
- **[`combined_cleanroom_boot_factory_app_64k.bin`](combined_cleanroom_boot_factory_app_64k.bin)**
  — working alternative combo (this project's own clean-room bootloader +
  the reconstructed factory app).
- **[`superseded/`](superseded/README.md)** — earlier-generation dumps and
  partial patches. Historical only, never flash.

## Safety

**Never flash anything in this repository to the live vehicle unit.** All
hardware testing described in this project's docs is against a spare
STM32F105RBT6 test board (e.g. a cheap CAN-filter device sharing the same
chip), connected via a Raspberry Pi Pico running CMSIS-DAP
(`tools/pico_stm32.cfg`).

## Extraction technical details

- **Probe**: Raspberry Pi Pico running Raspberry Pi `debugprobe` (CMSIS-DAP v2 mode).
- **Configuration**: `tools/pico_stm32.cfg` with `reset_config none` and `cortex_m reset_config vectreset`.
- **Exploit**: [`tools/stm32f1_extractor_fixed.py`](../../../tools/stm32f1_extractor_fixed.py), CVE-2020-8004 vector-table redirection over the unblocked Cortex-M3 ICode bus, used to read RDP1-locked flash word-by-word.
- **Reconstruction, bootloader**: [`tools/patch_factory_bootloader_live.py`](../../../tools/patch_factory_bootloader_live.py) — fills architecturally-unreachable gap words via disassembly-derived Thumb-2 opcodes, each with an inline comment citing the reasoning/evidence.
- **Reconstruction, application**: [`tools/patch_factory_app_live.py`](../../../tools/patch_factory_app_live.py) — same approach for the application partition.
- **Full narrative**: [`docs/BOOTLOADER_HANG_TRACE_2026-09-14.md`](../../../docs/BOOTLOADER_HANG_TRACE_2026-09-14.md) documents the complete, session-by-session trace from initial permanent hang through every bug found and fixed to the final working state.
