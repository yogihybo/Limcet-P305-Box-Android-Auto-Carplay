# Live STM32F105 MCU Firmware Dumps — Authenticated & Verified (September 2026)

> **STATUS: RESTORED & VERIFIED (2026-09-11).**  
> The earlier August 28th extraction attempt was retracted because of two compounding software flaws in `tools/stm32f1_extractor_fixed.py` (a broken `address % 0x200` shortcut and an oversized `table_size = 128` targeting non-existent interrupts).  
> On September 11, 2026, the adapter was transitioned from ST-Link HLA to a **Raspberry Pi Pico running CMSIS-DAP (`debugprobe`)**, unlocking native `cortex_m maskisr off` and `vectreset`. `tools/stm32f1_extractor_fixed.py` was corrected with `table_size = 64` (256 bytes) and proper exception wrap-around.  
> **Correction (2026-09-12):** the "100% zero artifacts" claim below was premature. Independent verification (`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` §5) found this dump genuinely is real hardware content -- confirmed reproducible across two independent runs, and disassembly-verified against the application firmware's own literal pool -- but it is **not** artifact-free: one word (`0x08001234`, expected `RCC_BASE`/`0x40021000`, actually `0x802CF8D8`) is a confirmed, root-caused extraction artifact (a Cortex-M `INVSTATE` UsageFault on a vector word with bit 0 = 0, not a fabrication). Only the ~5,700 bytes with an independent app-firmware cross-reference have been checked this thoroughly; the remainder of the bootloader has not been verified word-by-word. See §5 for the full methodology before citing "0 artifacts" anywhere else.
>
> **Also corrected (2026-09-12):** the real factory bootloader's own app-validation code (disassembled at `0x08001844`) checks for the application vector table at **`0x08003000`**, not `0x08004000` -- the address ranges below predate this finding and are being kept as originally written pending a full sweep; see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` §5.2.

---

## Dump Inventory

| File | Memory Range | Size | SHA-256 | Description |
| :--- | :---: | :---: | :--- | :--- |
| [`live_bootloader.bin`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_bootloader.bin) | `0x08000000` – `0x08003FFF` | 16 KB (16,384 B) | `6d32a969d0e4bd1b5a7dedbde0a8360b4c7dca1de727935ef00106ad6b36aa35` | Factory IAP Bootloader (USB OTG + USART update engine) |
| [`live_app_1302.bin`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | `0x08004000` – `0x0800FFFF` | 48 KB (49,152 B) | `381855df8ca4e9b2071cce02ae3a72bc03be4ecd0d474642417b69075a859b4d` | Authentic native **Limcet-V1.0-1302** Toyota Prado companion firmware |

---

## Forensic Verification

### 1. Bootloader (`live_bootloader.bin`)
* **Reset Vector & Entry Point**:
  `0x080004AC` executes `ldr r0, [pc, #36]` to read literal `0x080004D4` (`0x08000339` $\rightarrow$ `SystemInit`), identically matching the live BusFault address observed in early ST-Link tests.
* **Update Cookie Check**:
  At `0x080017E4`, the code loads `[0x20004004]` and compares against `0x5555AAAA` to enter YMODEM update mode.
* **Register-leak artifact class (the original `% 0x200` bug's signature) confirmed absent**: 0 words of `0x20000005` out of 4,096 total words. **However**, a *different*, narrower artifact class was found and root-caused on 2026-09-12 (one `INVSTATE`-fault word, `0x08001234`) -- see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` §5.1 before citing this as "zero artifacts, full stop."

### 2. Application Firmware (`live_app_1302.bin`)
* **Identity String**: Contains the authentic ASCII string:
  ```text
  Limcet-V1.0-1302
  ```
* **Authentic Toyota Target**: Completely distinct from the generic `DCn32-VOLVO-V2.10-20240909` USB package in the archive. Contains the physical Toyota Prado CAN ID tables, SWC mappings, and GPIO configurations.
* **Integrity Metrics**: 8,776 valid code/data words (71.4%), 2,835 erased flash words (`0xFFFFFFFF`), and 0 words of the original register-leak signature (`0x20000005`). Not independently word-by-word verified against a third reference the way the bootloader's correlating regions were -- see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md` §5.1's stated scope limits.

---

## Extraction Technical Details
* **Probe**: Raspberry Pi Pico running Raspberry Pi `debugprobe` (CMSIS-DAP v2 mode).
* **Configuration**: `tools/pico_stm32.cfg` with `reset_config none` and `cortex_m reset_config vectreset`.
* **Exploit Script**: Patched [`tools/stm32f1_extractor_fixed.py`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/tools/stm32f1_extractor_fixed.py) executing CVE-2020-8004 vector redirection over the unblocked Cortex-M3 ICode bus.
