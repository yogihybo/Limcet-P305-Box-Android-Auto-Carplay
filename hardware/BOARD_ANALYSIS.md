# Prado Head Unit — Board Hardware Analysis

**Board:** DC_LIMCET_MB_REV_003
**Photos taken:** 2026-06-26

See [`ark1668-limcet-prado.dts`](ark1668-limcet-prado.dts) for a structured
device-tree-style writeup combining this physical inspection with the kernel/
userspace RE findings in [`docs/1.1_HARDWARE_AND_SOC_REFERENCE.md`](../docs/1.1_HARDWARE_AND_SOC_REFERENCE.md)
— note the real firmware has no device tree at all (ATAG boot), so that file
is a documentation reconstruction, not a real boot artifact.

---

## Component Inventory

### Main SoC — ARK1668

| Field | Value |
|-------|-------|
| Marking | ARK1668 |
| Lot code | G210209927N |
| Package | BGA |

The silicon is marked **ARK1668**. The firmware, U-Boot, and kernel all reference `ARK1680` — this appears to be the product family name used in software while the actual silicon die is ARK1668. The two names refer to the same device.

Photos: `board_photo_01.jpg`, `board_photo_03.jpg`, `board_photo_07.jpg`

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

Photo: `board_photo_09.jpg`

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
| Enable pin | Linux `gpio91` — `BTEN_INTERFACE=gpio91` in `/etc/blueware.properties` |

The BT module communicates with the ARK1668 over a high-speed UART (`/dev/ttyHS1`, 1.5Mbps). Voice calls use UART audio routing (`VOICE_TYPE=UART`). The module handles HFP, A2DP, AVRCP, and iAP2 profiles.

Photo: `board_photo_04.jpg`

---

### WiFi

Realtek **RTL8811CU** — a separate chip from the BT module, internally wired to the SoC's `usb1` port (no external connector on that port). Confirmed via boot log across every available capture, no exceptions (`rtl8811cu` driver messages immediately after `usb 2-1: new high-speed USB device` — see `docs/1.4_WIRELESS_AND_INIT.md`). Not SDIO, despite MMC1's DTS comment calling it a "SDIO WiFi Controller" — that comment is confirmed wrong.

The rootfs carries five WiFi drivers total, for other board variants:

- `wlan_rtl8821cu.ko` (3.2MB)
- `wlan_rtl8821cs.ko` (2.5MB)
- `wlan_rtl8822cs.ko` (2.4MB)
- `wlan_rtl8811cu.ko` (2.3MB) — the confirmed chip on this hardware
- `wlan_rtl8189fs.ko` (1.2MB)

Only one loads at runtime depending on which chip is present; on this project's own unit that's `wlan_rtl8811cu.ko`. The WiFi is used exclusively as a software access point (`hostapd`) for Android Auto wireless and CarPlay wireless — it does not connect to external networks.

---

### Rear Camera Video Decoder — RN6752

| Field | Value |
|-------|-------|
| Part | RN6752 (Intersil / Renesas) |
| Function | CVBS analogue video decoder |
| Interface | ITU-656 digital video to ARK1668 |

Decodes the CVBS composite signal from the reversing camera and feeds it to the ARK1668 via ITU-656. The `itu656_load.sh` init script and `ITU656_BYP_*` sections in `arkdata.ini` relate to this path.

Photo: `board_photo_04.jpg`

---

### Display Adapter Board — DC_FUJITSU_CON96P_REV_002

A separate interposer PCB labelled **DC_FUJITSU_CON96P_REV_002** sits between the main board and the display panel. It adapts the main board's edge connector (CONN2 / CONN4) to a Fujitsu 96-pin FPC standard used by the LCD panel.

Photo: `board_photo_02.jpg`

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
- ~~Touch events (forwarded to ARK1668 — confirmed via MCU Monitor in advanced
  factory menu)~~ — **retracted (2026-07-11), see `docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md`.**
  Contradicted by: (1) direct on-screen observation that the MCU Monitor only
  shows CAN-bus activity; (2) a live `/dev/ttyHS0` byte capture showing zero
  traffic at all, not even the idle status frames expected regardless of
  touch; (3) `/etc/ts.conf` + `TSLIB_TSDEVICE=/dev/input/event0` in the stock
  rootfs, which authoritatively confirms tslib reads touch from a kernel
  evdev device (`gt9xx.ko`/`ark1680_ts.ko`), not any serial link. The
  MCU-forwards-touch claim was likely a misattribution from an earlier
  session and should not be relied on.
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

### GPIO Usage (ARK1668 / Linux side)

No public ARK1680/ARK1668 datasheet exists (see Key Findings in `docs/KERNEL.md`), and
neither the kernel nor U-Boot source is available in this repo — only the compiled
`zImage` (compressed, not string-searchable) and patched U-Boot binaries. What follows
was recovered by grepping plain-text configs and extracting ASCII strings from
uncompressed userspace binaries/libraries in the rootfs — it identifies which Linux
`gpioN` sysfs numbers are used and by what, but **cannot** map them to physical
ARK1668 package pins/pads without a datasheet.

| GPIO | Used by | Function | Evidence |
|------|---------|----------|----------|
| `gpio91` | `blueware` (Bluetooth daemon) | BT module enable | `BTEN_INTERFACE=gpio91` in `/etc/blueware.properties` |
| `gpio34` | `libCanBus.so` | Unconfirmed — see below | Literal shell commands `echo 0 > /sys/class/gpio/gpio34/value` / `echo 1 > /sys/class/gpio/gpio34/value` found in the binary |

**`gpio34` caveat:** despite being found in the CAN bus library, this is very likely
**not** the TJA1042 transceiver's enable/standby pin — that transceiver sits on the
MCU side (see above) and is controlled by the STM32, not by Linux. `libCanBus.so`
turns out to be a generic multi-vendor SDK for driving *external* aftermarket CAN
decoder boxes over UART (unused on this device — see
[`docs/1.2_CANBUS.md`](../docs/1.2_CANBUS.md#libcanbusso--a-separate-generic-multi-vendor-can-adapter-sdk)
for the full writeup), so `gpio34` is more plausibly a power/enable or presence-detect
line for that kind of external box than anything related to this unit's actual
onboard CAN circuit. Inference only — no string ties it to a specific purpose.

**`MsnCoreApp`** (the main application) has its own `CarSignalsWatch` class built on a
`GPIOOperater` abstraction (`getIONum`, `setValue`, `setEdge`, watched via
`onWatchGPIOThreadProc()` / log string `"Start Watch Car GPIO Signal:"`) — confirming
the Linux side does watch at least one GPIO for car-related signals directly, not
solely via UART from the MCU. Its specific pin number wasn't recoverable by string
search (likely a numeric literal in code, or read from a config file not present in
this rootfs dump) — would need actual disassembly to pin down.

Kernel-side: the `ark_gpio` driver and its `/proc/ark_gpio` debug interface, plus the
`ark_sys_pad_config_gpio_mode` pinmux function, are already documented as compiled-in
in `docs/KERNEL.md` — no specific pin assignments were recoverable from the compressed
kernel image itself.

---

### Serial Console Access

| Field | Value |
|-------|-------|
| Adapter | USB-TTL (PL-2303HX, lctech-inc.com) |
| Header location | 3-pin header near the SD card slot / right edge of board |
| Wiring | Yellow = TX, Blue = RX, Black = GND |
| Baud rate | 115200 (from U-Boot `baudrate=115200`) |

The UART console header is a separate physical connector on the board edge — it is **not** the USB-A port. The USB-TTL adapter plugs into a PC via USB-A and connects its UART wires to this header.

Photos: `board_photo_12.jpg`, `board_photo_13.jpg`, `board_photo_14.jpg`

---

### Power / Wiring Harness

| Field | Value |
|-------|-------|
| Power connector | Red multi-pin connector (top-right of board) |
| Harness connectors | Multiple black multi-pin connectors along bottom edge |
| Power management | DC-DC buck converter with 470µF/35V bulk caps and toroidal inductor |

The yellow/red/white multi-wire harness carries 12V ignition, battery, reverse trigger, and audio lines from the vehicle to the head unit.

Photos: `board_photo_15.jpg`, `board_photo_16.jpg`

---

## USB Port Note

The USB-A port on the board is a **host-mode** port for flash drives, CarPlay USB, and Android Auto USB. It is a separate connector from the serial console UART header.

The Holden base rootfs (`/etc/all.sh`) loads `g_zero.ko` — a USB gadget test driver that puts a USB controller into **device mode** at boot. This is the likely cause of the USB-A port not working as a host for flash drives or phone connections. Removing that `insmod` line from `all.sh` is the recommended fix.

See [`firmware_source/mtd6_rootfs/etc/all.sh`](../firmware_source/mtd6_rootfs/etc/all.sh) for the relevant line.
