# CAN Bus — Prado / Limcet-P306 (DC_LIMCET_MB_REV_003)

---

## Hardware

### CAN circuit

| Component | Part | Role |
|-----------|------|------|
| MCU | STM32F105RBT6 | 2× bxCAN controllers (CAN 2.0B active) |
| Transceiver | NXP TJA1042 | ISO 11898-2 high-speed CAN, up to 5 Mbit/s, CAN FD capable |

The TJA1042 is populated adjacent to the STM32F105 on the DC_LIMCET_MB_REV_003 board.
This is a complete, production CAN bus circuit — not an unpopulated option. The STM32
bxCAN controller (CAN_TX / CAN_RX on PA11/PA12 or PB8/PB9) connects to the TJA1042,
which drives the CANH/CANL differential pair out to the vehicle harness connector.

### Signal path

```
Toyota Prado steering wheel button press
  │
  ▼ vehicle body CAN bus (500 kbit/s, ISO 11898-2)
  │
  ├─ CANH ─┐
  │         ├── vehicle harness connector → head unit CAN input pins
  └─ CANL ─┘
              │
              ▼
         NXP TJA1042  (CANH/CANL ↔ differential, TXD/RXD ↔ 3.3V logic)
              │
              ▼
         STM32F105RBT6  bxCAN controller
              │  Limcet-V1.0-1302 firmware
              │  - initialises bxCAN at 500 kbit/s
              │  - filters for Toyota Prado SWC CAN IDs
              │  - decodes button byte from CAN data frame
              │  - sends key event over UART
              ▼
         /dev/ttyHS0  (UART to ARK1668, McuType=6 Limcet protocol)
              │
              ▼
         libMcuCenter.so  →  application key event
```

---

## Toyota Prado 150 CAN bus — SWC reference

Toyota Prado 150 series (2009–2021) uses the body CAN bus for steering wheel controls.

| Parameter | Value |
|-----------|-------|
| Bus speed | 500 kbit/s |
| Standard | ISO 11898-2 (high-speed CAN) |
| Typical SWC message IDs | `0x025`, `0x026` (varies by model year and region) |
| Frame type | Standard (11-bit ID) |
| Update rate | ~10–20 ms while button held |

**Note:** Toyota CAN IDs and byte layouts are not publicly standardised and can vary
between model years and markets. The values above are common references from community
reverse-engineering. Verify against a live capture for the specific vehicle.

---

## Diagnosis

### Step 1 — Check physical wiring

The CANH/CANL wires from the Prado factory harness connector must be connected to the
head unit's CAN input pins. This is the most common installation miss.

- On the Prado ISO/OEM harness, CAN is typically a **twisted pair**
- Wire colours vary by region and harness supplier (common: white/orange, green/yellow,
  or labelled CAN-H / CAN-L)
- Verify continuity from the harness plug to the head unit board CAN connector pins

### Step 2 — MCU Monitor (no tools needed)

With the harness connected and the vehicle ignition on:

1. On the head unit, open **Settings → Factory** (enter factory password)
2. Navigate to the advanced / MCU monitor screen
3. Press a steering wheel button and watch the raw data display

| Monitor result | Meaning | Next step |
|----------------|---------|-----------|
| Data bytes change on button press | MCU is decoding CAN and forwarding key events to ARK1668 | Check key mapping in software (see below) |
| No change at all | CAN frames not reaching MCU decoder | Check wiring, then MCU firmware CAN IDs |

### Step 3 — CAN bus capture (if MCU Monitor shows nothing)

Connect a USB CAN adapter (e.g. CANable, PEAK PCAN-USB) to the Prado OBD-II port or
harness CANH/CANL and capture with `candump` or SavvyCAN:

```sh
candump can0 -t a -x
```

Press each steering wheel button and note which CAN IDs change. Compare the IDs to
what the MCU firmware expects. If the IDs are present in the capture but MCU Monitor
still shows nothing, the MCU firmware CAN ID filter is wrong.

---

## MCU firmware (Limcet-V1.0-1302)

The CAN ID filter and button byte decoding is compiled into the STM32 firmware.
The ARK1668 software has no visibility into this — it only receives pre-decoded key
codes over `/dev/ttyHS0`.

### Checking the MCU firmware version

The firmware version is displayed on **Settings → About / System Info**:

```
MCU: Limcet-V1.0-1302
```

This was the version present when the device was last working. If the MCU firmware
has been inadvertently changed, this string will differ.

### Updating the MCU firmware

The `libMcuCenter.so` references MCU update file paths (`mcuupdate4/`, `mcu_update.bin`,
`msnmcu_update.bin`), suggesting the ARK1668 can push MCU firmware over `/dev/ttyHS0`
if the MCU supports the OTA bootloader protocol. However, no MCU update binary is
included in the Holden rootfs — a Limcet-specific update package would be needed.

Alternative reflash methods for the STM32F105:

| Method | Requirements | Notes |
|--------|--------------|-------|
| STM32 USB DFU | USB cable to board USB OTG port (if exposed) | Boot with BOOT0=1 |
| UART bootloader | Access to STM32 USART1 pins | STM32 built-in bootloader |
| SWD / JTAG | STLink v2 or J-Link probe | Full debug access, most reliable |

---

## Software — CAN type configuration

In `MsnProductInfo.ini`:

```ini
CanType=0
McuType=6
MCUPortName="/dev/ttyHS0"
```

`CanType=0` means the ARK1668 application does not manage CAN directly — all CAN
handling is delegated to the STM32 MCU firmware. This is correct for this hardware.
Do not change `CanType` unless switching to a different MCU adapter that handles CAN
at the application layer (e.g. `McuAdapter_BoxP230` for Honda XBS).

`McuType=6` selects the Limcet protocol adapter in `libMcuCenter.so`, which matches
the `Limcet-V1.0-1302` MCU firmware.

---

## Known CAN-capable MCU adapters (libMcuCenter.so)

Only one software adapter has CAN bus methods:

| Adapter | CAN methods | Vehicle |
|---------|-------------|---------|
| `McuAdapter_BoxP230` | `makeCanBusProtocol`, `sendCanBusKeyData`, `writeCanBusData`, `recvCanDatas`, `processSWCKey` | Honda XBS |

This adapter is Honda-specific. For the Prado (McuType=6 / Limcet), CAN decoding is
handled entirely within the STM32 MCU firmware — the ARK1668 software receives only
the decoded key codes.

---

## Summary — why SWC buttons don't work

| Cause | How to verify | Fix |
|-------|---------------|-----|
| CANH/CANL harness wires not connected | Visual inspection of harness connector | Connect the twisted-pair CAN wires |
| MCU firmware wrong Toyota CAN IDs | CAN capture shows SWC messages; MCU Monitor shows nothing | Reflash STM32 with correct firmware |
| CAN bus speed mismatch | Capture shows garbled/no frames | Verify MCU firmware baud rate = 500 kbit/s |
| Key code not mapped in ARK1668 | MCU Monitor shows changing data on button press | Check McuType=6 key mapping in libMcuCenter.so |
