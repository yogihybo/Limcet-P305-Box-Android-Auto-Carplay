# Live STM32F105 MCU Firmware Dumps — Authenticated & Verified (September 2026)

> **STATUS: RESTORED & VERIFIED (2026-09-11).**  
> The earlier August 28th extraction attempt was retracted because of two compounding software flaws in `tools/stm32f1_extractor_fixed.py` (a broken `address % 0x200` shortcut and an oversized `table_size = 128` targeting non-existent interrupts).  
> On September 11, 2026, the adapter was transitioned from ST-Link HLA to a **Raspberry Pi Pico running CMSIS-DAP (`debugprobe`)**, unlocking native `cortex_m maskisr off` and `vectreset`. `tools/stm32f1_extractor_fixed.py` was corrected with `table_size = 64` (256 bytes) and proper exception wrap-around.  
> Both the factory IAP bootloader and the authentic Toyota Prado application firmware were **100% dumped with zero artifact words (`0x20000005` = 0)**.

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
* **Zero Artifacts**: 0 words of `0x20000005` out of 4,096 total words (100% genuine code & natural flash padding).

### 2. Application Firmware (`live_app_1302.bin`)
* **Identity String**: Contains the authentic ASCII string:
  ```text
  Limcet-V1.0-1302
  ```
* **Authentic Toyota Target**: Completely distinct from the generic `DCn32-VOLVO-V2.10-20240909` USB package in the archive. Contains the physical Toyota Prado CAN ID tables, SWC mappings, and GPIO configurations.
* **Integrity Metrics**: 8,776 valid code/data words (71.4%), 2,835 erased flash words (`0xFFFFFFFF`), and 0 register leak artifacts.

---

## Extraction Technical Details
* **Probe**: Raspberry Pi Pico running Raspberry Pi `debugprobe` (CMSIS-DAP v2 mode).
* **Configuration**: `tools/pico_stm32.cfg` with `reset_config none` and `cortex_m reset_config vectreset`.
* **Exploit Script**: Patched [`tools/stm32f1_extractor_fixed.py`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/tools/stm32f1_extractor_fixed.py) executing CVE-2020-8004 vector redirection over the unblocked Cortex-M3 ICode bus.
