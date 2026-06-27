# Prado Head Unit — Board Hardware Analysis

**Board:** DC_LIMCET_MB_REV_003
**Photos taken:** 2026-06-26

---

## Component Inventory

### Main SoC — ARK1668

| Field | Value |
|-------|-------|
| Marking | ARK1668 |
| Lot code | G210209927N |
| Package | BGA |

The silicon is marked **ARK1668**. The firmware, U-Boot, and kernel all reference `ARK1680` — this appears to be the product family name used in software while the actual silicon die is ARK1668. The two names refer to the same device.

Photos: `PXL_20260626_035809498.jpg`, `PXL_20260626_035849459.MP.jpg`, `PXL_20260626_040133926.jpg`

---

### NAND Flash — Toshiba TC58BVG0S3HTA00

| Field | Value |
|-------|-------|
| Manufacturer | Toshiba |
| Part number | TC58BVG0S3HTA00 |
| Lot | NT7979, Taiwan, 18239 AE |
| Capacity | 1Gbit = **128MB** SLC NAND |
| Mounting | Daughter module soldered to main board |

Confirms U-Boot output `NAND: 128 MiB`. The NAND is on a small sub-module (the "Limcet Box" compute module) rather than mounted directly on the main PCB.

Photo: `PXL_20260626_040239651.jpg`

---

### Bluetooth Module — Feasycom FSC-BT8251 V1.1

| Field | Value |
|-------|-------|
| Module | FSC-BT8251 V1.1 |
| Manufacturer | Feasycom |
| RF chip | Realtek (RTL-series BT SoC) |
| UART | `/dev/ttyHS1` at 1.5Mbps (from `blueware.properties`) |
| Config | `MODULE_TYPE=BW121` in `/etc/blueware.properties` |
| BT MAC suffix | `FC9F` (from `DC0D3014FC9F` seen in bootlog) |

The BT module communicates with the ARK1668 over a high-speed UART (`/dev/ttyHS1`, 1.5Mbps). Voice calls use UART audio routing (`VOICE_TYPE=UART`). The module handles HFP, A2DP, AVRCP, and iAP2 profiles.

Photo: `PXL_20260626_035858475.jpg`

---

### WiFi

Realtek WiFi chip is co-located on or near the BT module. The rootfs carries five WiFi drivers:

- `wlan_rtl8821cu.ko` (3.2MB)
- `wlan_rtl8821cs.ko` (2.5MB)
- `wlan_rtl8822cs.ko` (2.4MB)
- `wlan_rtl8811cu.ko` (2.3MB)
- `wlan_rtl8189fs.ko` (1.2MB)

Only one will be loaded at runtime depending on which chip is present. The WiFi is used exclusively as a software access point (`hostapd`) for Android Auto wireless and CarPlay wireless — it does not connect to external networks.

---

### Rear Camera Video Decoder — RN6752

| Field | Value |
|-------|-------|
| Part | RN6752 (Intersil / Renesas) |
| Function | CVBS analogue video decoder |
| Interface | ITU-656 digital video to ARK1668 |

Decodes the CVBS composite signal from the reversing camera and feeds it to the ARK1668 via ITU-656. The `itu656_load.sh` init script and `ITU656_BYP_*` sections in `arkdata.ini` relate to this path.

Photo: `PXL_20260626_035858475.jpg`

---

### Display Adapter Board — DC_FUJITSU_CON96P_REV_002

A separate interposer PCB labelled **DC_FUJITSU_CON96P_REV_002** sits between the main board and the display panel. It adapts the main board's edge connector (CONN2 / CONN4) to a Fujitsu 96-pin FPC standard used by the LCD panel.

Photo: `PXL_20260626_035822389.jpg`

---

### MCU — STM32F105RBT6

| Field | Value |
|-------|-------|
| Part | STM32F105RBT6 |
| Manufacturer | STMicroelectronics |
| Core | ARM Cortex-M3, up to 72 MHz |
| Flash | 128 KB |
| RAM | 64 KB |
| Package | LQFP64 |
| Firmware | Limcet-V1.0-1302 (from version screen) |
| UART to ARK1668 | `/dev/ttyHS0` |

**Key peripherals on the STM32F105RBT6:**
- **2× bxCAN** — hardware CAN bus controllers (CAN 2.0B active). CAN capable if a transceiver IC is present on the board.
- **12-bit ADC, 16 channels** — used to read the SWC (steering wheel control) resistor ladder voltage
- **USB OTG full-speed** — may be used for MCU firmware updates
- **3× USART** — one used for `/dev/ttyHS0` link to ARK1668
- **GPIO** — ACC/IGN detection, reverse trigger input, panel button inputs

The MCU runs the `Limcet-V1.0-1302` custom firmware which handles:
- Touch events (forwarded to ARK1668 — confirmed via MCU Monitor in advanced factory menu)
- Steering wheel button presses (ADC voltage divider on SWC input wire)
- Panel button inputs
- ACC/IGN signal processing
- Reverse trigger detection
- Firmware version reporting to the about screen

**CAN bus confirmed:** An **NXP TJA1042** CAN transceiver is populated adjacent to the
STM32F105 on the DC_LIMCET_MB_REV_003 board.

| Field | Value |
|-------|-------|
| Part | NXP TJA1042 |
| Type | High-speed CAN transceiver, ISO 11898-2 |
| Speed | Up to 5 Mbit/s (CAN FD and Classic CAN) |
| Package | SOIC-8 |
| Supply | 3.3V / 5V compatible |
| Features | Standby mode, remote wake-up via CAN |

The TJA1042 bridges the STM32F105 bxCAN controller (PA11/PA12 or PB8/PB9) to the
vehicle CANH/CANL lines. This is a complete, active CAN bus circuit. The MCU firmware
(Limcet-V1.0-1302) reads steering wheel button presses directly from the Toyota Prado
CAN bus — not from an ADC voltage divider on a dedicated SWC wire.

**SWC implication:** The `EnableSWCSwitchHardware` / ADC key-learning path in the
ARK1668 software is NOT what controls steering wheel buttons on this device. The MCU
firmware decodes Toyota-specific CAN messages and translates them to key events sent
to the ARK1668 over `/dev/ttyHS0`.

---

### Serial Console Access

| Field | Value |
|-------|-------|
| Adapter | USB-TTL (PL-2303HX, lctech-inc.com) |
| Header location | 3-pin header near the SD card slot / right edge of board |
| Wiring | Yellow = TX, Blue = RX, Black = GND |
| Baud rate | 115200 (from U-Boot `baudrate=115200`) |

The UART console header is a separate physical connector on the board edge — it is **not** the USB-A port. The USB-TTL adapter plugs into a PC via USB-A and connects its UART wires to this header.

Photos: `PXL_20260626_044832124.jpg`, `PXL_20260626_053749292.jpg`, `PXL_20260626_053757555.jpg`

---

### Power / Wiring Harness

| Field | Value |
|-------|-------|
| Power connector | Red multi-pin connector (top-right of board) |
| Harness connectors | Multiple black multi-pin connectors along bottom edge |
| Power management | DC-DC buck converter with 470µF/35V bulk caps and toroidal inductor |

The yellow/red/white multi-wire harness carries 12V ignition, battery, reverse trigger, and audio lines from the vehicle to the head unit.

Photos: `PXL_20260626_075047263.jpg`, `PXL_20260626_075106810.NIGHT.jpg`

---

## USB Port Note

The USB-A port on the board is a **host-mode** port for flash drives, CarPlay USB, and Android Auto USB. It is a separate connector from the serial console UART header.

The Holden base rootfs (`/etc/all.sh`) loads `g_zero.ko` — a USB gadget test driver that puts a USB controller into **device mode** at boot. This is the likely cause of the USB-A port not working as a host for flash drives or phone connections. Removing that `insmod` line from `all.sh` is the recommended fix.

See [`../rootfs/etc/all.sh`](../rootfs/etc/all.sh) for the relevant line.
