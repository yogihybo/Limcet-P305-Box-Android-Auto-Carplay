# Engineering Handoff: Rohm BD37033 & Hardware I2C Architecture

**Date**: August 27, 2026  
**Scope**: Focused technical summary of the Rohm BD37033 sound processor, I2C routing, LCD pin conflict resolution, and hardware controller activation.

---

## 1. Physical Hardware Observation

* **Chip Present**: The Rohm **BD37033FV** (5.1-channel digital sound processor, SOP-28 package) is physically populated and soldered on the PCB.
* **PCB Traces**: Physical traces from **Pin 27 (SDA)** and **Pin 28 (SCL)** run across the board and disappear directly under the **ArkMicro ARK1668 BGA SoC**.

---

## 2. Root Cause of the Legacy "LCD Pin Conflict"

In early reverse-engineering passes, the devicetree drafted software bit-banged I2C buses (`i2c-gpio-0` on `GPIO 2/3` and `i2c-gpio-1` on `GPIO 9/121`). 

### Why That Assumption Was Flawed:
Cross-referencing the official ARK1668 pin-multiplexing definitions (`ark1668-pinctrl.dtsi`) reveals that these GPIOs are actually the **LCD Controller's Red Pixel Bus**:

| Pin / GPIO | False Device Tree Assignment | Real ARK1668 BGA Hardware Function |
|---|---|---|
| **`Bank 0 Pin 2` (GPIO 2)** | `i2c-gpio-0` SCL | **`LCD_D0` (Red Bit 0: `r0`)** |
| **`Bank 0 Pin 3` (GPIO 3)** | `i2c-gpio-0` SDA | **`LCD_D1` (Red Bit 1: `r1`)** |
| **`Bank 0 Pin 9` (GPIO 9)** | `i2c-gpio-1` SDA | **`LCD_D7` (Red Bit 7: `r7` - Red Most Significant Bit)** |

* **The Problem**: Whenever the kernel tried to probe `i2c-gpio-1` on `GPIO 9`, it reconfigured the LCD's Red MSB pixel line as a GPIO input/output, corrupting the RGB888 pixel stream to the Fujitsu LCD panel (causing severe red-channel color distortion and display freezes).
* **The Reality**: The BD37033 is **not** wired to the LCD display lines.

---

## 3. The True Hardware I2C Destination

The ARK1668 SoC contains a **dedicated hardware I2C controller (`i2c0` at `0xe4300000`)** on dedicated peripheral pads that have **zero connection or overlap with the LCD display matrix**:

| I2C Signal | Physical ARK1668 BGA Pad | GPIO Number | Hardware Peripheral Base |
|---|---|:---:|:---:|
| **`SCL` (Clock)** | **Bank 2 Pin 6** | `GPIO 70` | `0xe4300000` (`i2c0`) |
| **`SDA` (Data)** | **Bank 2 Pin 7** | `GPIO 71` | `0xe4300000` (`i2c0`) |

* **Hardware Independence**: Communicating through `i2c0` (`/dev/i2c-0`) operates completely in parallel with the LCDC display engine without any risk of screen tearing, color tinting, or pin conflicts.

---

## 4. BD37033 Power & Addressing Requirements

When probing the physical chip over hardware `i2c0`, keep in mind:

1. **Power-On-Reset (POR) Behavior**:
   * The BD37033 is an active CMOS silicon IC. Without active I2C programming, all internal channels power up in **Full Mute ($-\infty\,\text{dB}$ / `0xFF`)** and input switches are open. It cannot pass audio passively.
2. **Gated Power Rail ($V_{CC}$ / $V_{DD}$)**:
   * Requires $+8\text{V}$–$+10\text{V}$ on Pin 15 ($V_{CC}$) and $+3.3\text{V}$ on Pin 17 ($V_{DD}$).
   * In stock drivers (`libMsnSound.so` `0xa5a4`), **`GPIO 34`** is configured as an output power gate. Ensure `GPIO 34` is asserted so the analog power rail is energized.
3. **I2C Slave Address**:
   * **`ADR` Pin (Pin 26) tied to GND**: 7-bit Address = `0x40` (8-bit write `0x80`).
   * **`ADR` Pin (Pin 26) tied to VDD**: 7-bit Address = `0x41` (8-bit write `0x82`).

---

## 5. Concrete Next Steps to Activate & Validate

1. **Update Device Tree (`hardware/ark1668-limcet-prado.dts`)**:
   * Enable the hardware I2C controller node:
     ```dts
     &i2c0 {
         status = "okay";
         pinctrl-names = "default";
         pinctrl-0 = <&pinctrl_i2c0>;  /* Bank 2 Pin 6 (SCL), Bank 2 Pin 7 (SDA) */
         clock-frequency = <100000>;

         amp: bd37033@40 {
             compatible = "arkmicro,drv_bd37033";
             reg = <0x40>;
             status = "okay";
         };
     };
     ```
   * Remove legacy `i2c-gpio-0` and `i2c-gpio-1` nodes to ensure LCD pins (`r0`, `r1`, `r7`) remain untouched.
2. **Compile & Deploy DTB**:
   ```bash
   make dtbs
   ```
3. **Live Hardware Verification**:
   * Boot the kernel and scan the hardware bus:
     ```bash
     i2cdetect -y 0
     ```
   * Confirm the BD37033 answers at address `0x40` (or `0x41`) with zero display artifacts.
