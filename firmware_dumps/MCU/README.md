# Limcet DCn32 Companion Microcontroller Binaries

This directory catalogs all official **Limcet `DCn32` series companion MCU firmware images** extracted from firmware dump archives. All binaries in this series run on the **STM32F105** microcontroller, execute from Flash base **`0x08004000`** (16 KB IAP bootloader window), and communicate with the ArkMicro Linux SoC over `/dev/ttyHS0` at 38400 baud using the standard `0x2E` packet protocol.

---

## Catalogued Versions

### 1. `DCn32-VOLVO-V2.10-20240909`
* **Path**: `DCn32-VOLVO-V2.10-20240909/can_app.bin`
* **Size**: 31,996 bytes
* **MD5**: `bea19bfef83f50de1f91c217aa3c2cc5`
* **Vector Table**: SP: `0x20002DC0`, Reset: `0x08004239`
* **Target / Features**: September 2024 production build. Full Toyota Prado 150 CAN decoding (`0x3C4`), ADC touch relay switching, and mute lines.

### 2. `DCn32-VOLVO-V2.10-20240418`
* **Path**: `DCn32-VOLVO-V2.10-20240418/can_app.bin`
* **Size**: 31,804 bytes
* **MD5**: `612224533191d5dc889926553f582c3d`
* **Vector Table**: SP: `0x20002DC0`, Reset: `0x08004239`
* **Target / Features**: April 2024 production build for Limcet Silver Box (with Globe icon, 2015+ models).

### 3. `DCn32-VOLVO-V3.00-20240403`
* **Path**: `DCn32-VOLVO-V3.00-20240403/can_app.bin`
* **Size**: 31,964 bytes
* **MD5**: `0c6368526d0e2084591e5fe0570c1fd3`
* **Vector Table**: SP: `0x20002DD8`, Reset: `0x08004239`
* **Target / Features**: April 2024 V3.00 build for Limcet Silver Box (without Globe, pre-2014). Includes Bluetooth AT device name configuration (`AT+IGNAME=Limcet`).

### 4. `DCn32-ACURA-V1.01-20250409`
* **Path**: `DCn32-ACURA-V1.01-20250409/can_app.bin`
* **Size**: 24,624 bytes
* **MD5**: `e38e5a3c05bab14fcf3291789aabf9d2`
* **Vector Table**: SP: `0x20002B48`, Reset: `0x08004239`
* **Target / Features**: April 2025 build for **Honda Odyssey / Acura** (`奥德赛支持右转盲区摄像头`). Supports automatic video relay switching for the Honda LaneWatch passenger-side blind spot mirror camera (`GPIOC Pin 13 / Pin 2`) on right-turn signal detection.
