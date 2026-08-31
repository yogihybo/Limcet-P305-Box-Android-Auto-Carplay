# Engineering Handoff: MCU Signals, Audio Pipeline & BD37033 Hardware I2C

**Date**: August 27, 2026  
**Status**: `master` branch up-to-date and verified on commit `ec8c2b19`

---

## 1. Executive Summary & Session Accomplishments

1. **MCU Inbound/Outbound Protocol Audit & Android Auto Integration**:
   - Audited all MCU wire codes (`CMD 0x01`, `0x02`, `0x03`, `0x04`, `0x05`, `0x12`, `0x20`, `0xA0`).
   - Verified that all active vehicle controls (Touch digitizer, Rotary knob focus, Home/Media/Call buttons, Headlight/Night mode, Reverse camera overlay, and Phone clock sync) are **fully implemented and verified in the codebase**.
   - Confirmed car wheel speed is **strictly excluded** from Android Auto, ensuring navigation apps rely 100% on the phone's native GPS.
2. **Climate CAN (`0x240`) & Handbrake Disassembly Findings**:
   - Confirmed that `can_app.bin` (`0x0800BB58`) contains full decoding logic for Toyota CAN ID `0x240` (dual-zone HVAC, temperatures, fan speed, A/C state).
   - Documented that `CMD 0x05` (Subtype 1) and handbrake `CMD 0x03` only transmit when live state-change frames arrive over the physical CAN bus in the vehicle.
3. **Dual-Microphone Hardware Architecture & UI Toggle**:
   - Disassembled stock `libMcuCenter.so` (`MCUAdapter_ZhongHang::syncSettingDataToMcu` / `0x0009B220`) and MCU firmware `can_app.bin` (`0x08005AA0`).
   - Identified the onboard 2:1 analog audio multiplexer gated by the MCU's **`GPIOB Pin 6`**.
   - Added interactive **"OEM Factory Microphone"** toggle in `custom_ui` (`Settings -> AUDIO`), syncing `CMD 0xA0 [0x09, 0x01/0x00]` on change and on boot.
   - Identified the active power requirement: Toyota Prado 150 roof console mic requires $+5\text{V}$ on **Pin 6 (`MACC`)** of the 28-pin radio connector.
4. **Rohm BD37033 Physical I2C Routing & LCD Conflict Resolution**:
   - Disproved the legacy assumption that BD37033 is on software `i2c-gpio` (GPIO 2/3/9/121), which hijacked the LCD's Red pixel color bits (`r0`, `r1`, `r7`) and caused display corruption.
   - Confirmed physical traces running under the BGA package route to the ARK1668's **dedicated hardware I2C0 controller (`0xe4300000` / Bank 2 Pin 6 `SCL`, Bank 2 Pin 7 `SDA`)**, completely isolated from the LCD.

---

## 2. Hardware Architecture & Signal Map

```
                             +---------------------------------------+
                             |   ARK1668 Main Processor (ARM-A5)     |
                             |                                       |
                             |   [ Linux 4.19.192 / ALSA sdadc ]     |
                             |   [ Direct Internal DAC (ARK-SDDAC) ] |
                             |   [ HW I2C0 Controller (0xe4300000) ] |
                             +--------+--------------+---------+-----+
                                      |              |         |
                  SoC MICIN (Analog)  |       UART   |         | HW I2C0 (Bank 2 Pin 6/7)
                                      |   /dev/ttyHS0|         | (GPIO 70/71)
                                      v              v         v
           +--------------------------+----+   +-----+---------+-----+
           | 2:1 Analog Multiplexer IC     |   | STM32F105 Companion |
           | (SGM3157 / TS5A3159)          |   | MCU (can_app.bin)   |
           +------------+------------------+   +----------+----------+
                        ^                                 |
       IN0 [PB6 = 0]    |   IN1 [PB6 = 1]                 | PB6 Control Gate
     +------------------+------------------+              | (CMD 0xA0 [0x09])
     |                                     |              v
3.5mm External Jack            Toyota 28-Pin Harness   [Audio Multiplexer]
(Aftermarket Electret Mic)     (Factory Roof Mic)
                               Pin 4: MIC+
                               Pin 5: MIC-
                               Pin 6: MACC (+5V DC)
```

---

## 3. MCU Command & GPIO Reference

### Dedicated STM32 MCU GPIO Map (`hardware/MCU/can_app.bin`)

> **CORRECTION (2026-08-31), copied from `docs/1.3.1_MCU_FIRMWARE_
> DECOMPILATION.md`'s own correction of this same table (both docs
> carried the identical error, likely copy-pasted from one source):**
> four rows below (`0x0800599C`, `0x080059B0`, `0x080059D8`,
> `0x080059E8`) are wrong -- both the GPIO port and the function type.
> All four are real, confirmed **input reads** (`ldr r3,[r2,#8]`, the
> STM32 `IDR` register offset -- architecturally exclusive to reads,
> never output control), not the output/power-gate writes claimed
> here. Real port+pin: `0x0800599C` = `GPIOA Pin 8` (not `GPIOA Pin
> 1`), `0x080059B0` = `GPIOC Pin 9` (not `GPIOA Pin 9`), `0x080059D8` =
> `GPIOC Pin 7` (not `GPIOA Pin 7`), `0x080059E8` = `GPIOB Pin 2` (not
> `GPIOA Pin 2`) -- the last one is the real source `flag_5e` gates on,
> see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s "flag_5e, fully
> traced" section for the full chain. The "Power Amp Mute"/"USB 5V
> Power Rail"/"AM/FM Radio Tuner Power"/"Bluetooth Module Power"
> *labels* for these four rows are consequently also unconfirmed. The
> other rows were not re-checked this pass.

| Pin | Peripheral / Target | Firmware Function | Hardware Behavior |
|---|---|:---:|---|
| **`GPIOB Pin 6`** | **Microphone / Audio Mux Switch** | `0x08005AA0` | `0` = Aftermarket 3.5mm Jack (`CMD 0xA0 [0x09, 0x00]`); `1` = Toyota Factory Roof Mic (`CMD 0xA0 [0x09, 0x01]`). |
| **`GPIOC Pin 13`** | **OEM Camera Bypass Relay** | `0x080058F8` | High = Asserts mechanical bypass relay for Toyota OEM camera/nav; Low = Aftermarket camera. |
| **`GPIOC Pin 2`** | **CVBS Video Multiplexer** | `0x0800591C` | Selects composite video input path to RN6752 ITU-656 decoder. |
| **`GPIOB Pin 0`** | **CBT16211A Touch Switch** | `0x08005A3C` | High = Closes touch bus switch connecting resistive digitizer lines to MCU ADC. |
| ~~**`GPIOA Pin 1`** | **Power Amp Mute (`PA_MUTE`)**~~ | `0x0800599C` | **WRONG, see correction above.** |
| ~~**`GPIOA Pin 9`** | **USB 5V Power Rail**~~ | `0x080059B0` | **WRONG, see correction above.** |
| **`GPIOA Pin 8`** | **LCD Backlight PWM / Enable** | `0x080059C0` | Enables backlight boost converter / PWM modulation. (A different, nearby address from the corrected rows -- not itself re-verified this pass.) |
| ~~**`GPIOA Pin 7`** | **AM/FM Radio Tuner Power**~~ | `0x080059D8` | **WRONG, see correction above.** |
| ~~**`GPIOA Pin 2`** | **Bluetooth Module Power**~~ | `0x080059E8` | **WRONG, see correction above -- this is the real `flag_5e` source.** |
| **`GPIOB Pin 5`** | **LCD Panel Reset Line** | `0x080059F8` | Hardware reset strobe to Fujitsu 96-pin LCD panel. |
| **`GPIOA Pin 14`**| **SoC Hardware Reset Strobe** | `0x08005A18` | Hardware reset trigger to ARK1668 main processor. |
| **`GPIOA Pin 15`**| **Piezo Reverse Warning Buzzer**| `0x08005A7C` | Generates audible parking sensor / reverse alert beeps. |
| **`GPIOB Pin 4`** | **Status Indicator LED** | `0x08005A5C` | Flashes system status / heartbeat indicator. |

---

## 4. Key Takeaways: Microphone Input

1. **Why the 3.5mm Aftermarket Mic Works**:
   - Plugs into the 3.5mm pigtail jack, routed via `GPIOB Pin 6 = 0` directly to SoC `MICIN` with $3.3\text{V}$ bias.
   - Captured by ALSA `plughw:0,1` (`sdadc`).
2. **Why the Factory Toyota Roof Mic Was Silent**:
   - The default MCU state is `0` (3.5mm mic). Toggling `Settings -> AUDIO -> OEM Factory Microphone` in `custom_ui` sends `CMD 0xA0 [0x09, 0x01]` to flip `GPIOB Pin 6` to the 28-pin harness pins.
   - **Crucial Hardware Note**: The factory roof mic requires active $+5\text{V}$ DC power on **Pin 6 (`MACC`)** of the 28-pin Toyota connector to power its ceiling FET pre-amplifier.
3. **No Kernel Updates Needed**:
   - Both microphones feed into the same SoC `MICIN` pin through the hardware switch. The kernel captures both through the same ALSA device (`plughw:0,1`).

---

## 5. Next Steps for Hardware I2C & BD37033

1. **Update Device Tree (`hardware/ark1668-limcet-prado.dts`)**:
   - Enable dedicated hardware `&i2c0` (`0xe4300000`) with `pinctrl_i2c0` (Bank 2 Pin 6 / Pin 7).
   - Remove conflicting software `i2c-gpio-0` / `i2c-gpio-1` nodes to prevent stealing LCD Red pixel lines (`r0`, `r1`, `r7`).
2. **Live Device Validation**:
   - Compile new DTB with `make dtbs`.
   - Boot device and run `i2cdetect -y 0` to verify BD37033 ACK at address `0x40` without any LCD artifacts.
