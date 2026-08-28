# Live STM32F105 MCU Firmware Dumps

These files are the **true, live hardware firmware extractions** dumped directly from the companion STM32F105 microcontroller on the physical Prado head unit using OpenOCD and the CVE-2020-8004 exception exploit (`tools/stm32f1-firmware-extractor`):

---

## **Dump Inventory**

| File | Memory Range | Size | SHA-256 | Description |
|---|---|---|---|---|
| [`live_bootloader.bin`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_bootloader.bin) | `0x08000000` – `0x08003FFF` | $16\text{ KB}$ | `73669d5f443a4c320e3be263ee6df4d2f0e30fe2b0afdd49e0781e13dfd63c22` | Factory IAP Bootloader (USB OTG FS + USART1 DFU engine) |
| [`live_app_1302.bin`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_app_1302.bin) | `0x08004000` – `0x0800BFFF` | $32\text{ KB}$ | `183f535d70fd1791701508ecd02ac5132c69741b506d6dfd0c5402e0ed4e0577` | Live **Limcet-V1.0-1302** native Toyota/Prado companion application |
| [`live_eeprom_nvram.bin`](file:///home/osboxes/Downloads/prado-firmware-reconstruction/hardware/MCU/live_dumps/live_eeprom_nvram.bin) | `0x0801F000` – `0x0801FFFF` | $4\text{ KB}$ | `8b0b97779d71c55d045d65600c3b036573c09f7a77e5d8ff695781a547285a97` | Emulated EEPROM / High-flash calibration and NVRAM sector |

---

## **Extraction Method**

* **Debug Interface**: SWD (PA13/SWDIO, PA14/SWCLK).
* **Protection Bypass**: STM32F1 RDP Level 1 bypass via ARM Cortex-M3 Vector Table Offset Register (`VTOR` / `0xE000ED08`) exception handler instruction leaking over the ICode bus (CVE-2020-8004).

---

## **Hardware Architecture & Subsystem Analysis**

### **1. Audio & Power Architecture**
* **Direct DAC Path (`SoundType=0`)**: The ArkMicro SoC's internal stereo DAC (`plughw:0,0` / `e4000000.sddac`) connects directly via AC-coupling capacitors to the Toyota OEM radio/amplifier Line-In/AUX input.
* **Software Volume Scaling**: Because the DAC bypasses MCU analog attenuation, volume, muting, and equalizer controls are executed directly in software on the Linux host (within `custom_ui` / `aasdk` / ALSA PCM pipeline).
* **MCU Power Rails**: The MCU provides hardware gating on `GPIOA Pin 7` (Audio $+8.5\text{V}$ rail enable) and `GPIOA Pin 1` (PA mute), activated via `CMD 0x84 [0x00, 0x03]`.

### **2. STM32 I2C2 Peripheral (`0x40005800`)**
* `live_app_1302.bin` implements the dedicated STM32 **hardware `I2C2` driver (`0x40005800`)** on **`PB10` (SCL)** and **`PB11` (SDA)** for communicating with the ROHM BD37033 sound processor (if populated).
* The Linux SoC does not touch the BD37033 I2C lines directly, but dispatches high-level setting commands (`CMD 0xA0`) to the MCU over `/dev/ttyHS0` at 38400 baud.

### **3. Microphone Multiplexer (`GPIOB Pin 6`)**
* **Switch Line**: Controlled by the MCU's `GPIOB Pin 6` (`0x08005AA0`).
* **OEM Mode (`GPIOB Pin 6 = 1`)**: Routes the 28-pin Toyota factory harness roof microphone into the SoC SAR-ADC (`plughw:0,1`). Gated via `CMD 0xA0 [0x09, 0x01]`.
* **Aftermarket Mode (`GPIOB Pin 6 = 0`)**: Routes the 3.5mm pigtail jack to the SoC SAR-ADC (`plughw:0,1`). Gated via `CMD 0xA0 [0x09, 0x00]`.
