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

### SWC frame layout (Toyota — community reference)

Frame ID `0x3C4` (may vary by model year and market):

| Byte 0 | Byte 1 | Button |
|--------|--------|--------|
| `0x00` | `0x00` | No button pressed (idle) |
| `0x80` | `0x00` | Volume Up |
| `0x40` | `0x00` | Volume Down |
| `0x04` | `0x00` | Mode / Source |
| `0x10` | `0x00` | Next Track |
| `0x08` | `0x00` | Previous Track |
| `0x00` | `0x80` | Phone pick-up |
| `0x00` | `0x04` | Enter / OK |

Byte 0 and Byte 1 are bitmask fields — multiple bits can be set simultaneously for
combo presses. The idle frame (`0x00 0x00`) is broadcast at ~10–20 ms intervals while
no button is held.

**Verify against your vehicle:** Toyota CAN IDs and byte layouts are not publicly
standardised and vary by model year and market. The table above is a community
reference. Capture live frames from the Prado harness to confirm the exact ID and
byte layout for this specific vehicle before programming any MCU firmware.

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

## Root cause — Holden firmware stripped CAN SWC support

SWC was confirmed working with the original Prado firmware (Limcet-P306 V3.10.3.0212).
After installing the Holden base firmware the SWC stopped working.

### What the library comparison revealed

Binary diff of the two `libMcuCenter.so` files (Prado: 721,912 bytes vs Holden: 586,384 bytes)
and `libCanBus.so` (Prado: 787,256 bytes vs Holden: 704,868 bytes) and `libSetting.so`
(Prado: 735,336 bytes vs Holden: 656,932 bytes) revealed the Holden firmware **deliberately
stripped the CAN SWC subsystem** and replaced it with IR remote learning:

| Feature | Prado original | Holden firmware |
|---------|---------------|-----------------|
| `MCUAdapter_RuiYuanSWC` class | ✅ Present | ❌ Removed |
| CAN key code tables (`0x7`,`0x8`,`0x9`…) | ✅ Present | ❌ Removed |
| `CanBusKeyManager` with `loadConfigs()` | ✅ Present | ❌ Removed |
| `CanBusKey.config` / `CanBusKeyMaps-%1` | ✅ Present | ❌ Removed |
| `on_btnCanBusSetting_clicked()` factory UI | ✅ Present | ❌ Removed |
| `radioCanBusBtn` / `radioMCUBtn` toggle | ✅ Present | ❌ Removed |
| `"Learning CanBus key:"` prompt | ✅ Present | ❌ Removed |
| `CanBus_Raise_Volkswagen` / `_Jeep` adapters | ✅ Present | ❌ Removed |
| `on_btnIRKeyLearn_clicked()` (IR remote) | ❌ Not present | ✅ Added |
| FM Transmitter (`qnd_*` chip commands) | ❌ Not present | ✅ Added |

The Holden firmware variant was built for a vehicle that uses an IR blaster SWC (not CAN bus).
The Prado uses CAN bus SWC, so the Holden firmware was entirely incompatible.

### Fix applied — Prado libraries restored

Three libraries from the Prado original rootfs (`mtd6_rootfs.bin`) have been copied into
the reconstruction rootfs:

| Library | Prado size | Holden size | Key restoration |
|---------|-----------|-------------|-----------------|
| `libMcuCenter.so` | 721,912 B | 586,384 B | `MCUAdapter_RuiYuanSWC`, CAN key tables |
| `libCanBus.so` | 787,256 B | 704,868 B | `CanBusKeyManager`, `CanBusKey.config` loader |
| `libSetting.so` | 735,336 B | 656,932 B | Full CAN SWC factory UI |

### Confirming the MCU is sending events (before FK Learn)

Use the MCU Monitor in the factory menu to confirm the MCU side is working:

1. **Settings → Factory → MCU Monitor** (advanced menu)
2. With the vehicle running and CAN connected, press a steering wheel button
3. Watch for changing byte values in the monitor display

| Monitor result | Interpretation |
|----------------|---------------|
| Bytes change on button press | MCU is decoding CAN and forwarding key events — issue is in ARK1668 app handling |
| No change at all | CAN wiring issue, CAN ID mismatch, or MCU not initialised |

If the monitor shows no change, diagnose the CAN layer first (see Step 3 above, CAN capture).

### Enable MCU debug logging

For deeper diagnosis via serial console or ADB shell:

```sh
touch /data/mcudebug_flag
# Reboot or restart the MCU application
# libMcuCenter.so logs MCU UART traffic to the system log
```

Raw UART capture:

```sh
cat /dev/ttyHS0 | hexdump -C &
# Press SWC buttons and observe hex output for key event packets
```

---

## Summary — why SWC buttons don't work

| Cause | Priority | How to verify | Fix |
|-------|----------|---------------|-----|
| **MCU protocol / init mismatch** — Holden libMcuCenter.so may not initialise Limcet-V1.0-1302 correctly | **1st** | MCU Monitor shows no change on button press; raw /dev/ttyHS0 capture shows no key events | Restore original Prado libMcuCenter.so or capture UART to diagnose init sequence |
| CANH/CANL harness wires not connected | 2nd | MCU Monitor shows no change; CAN capture shows no SWC frames | Connect the twisted-pair CAN wires from harness |
| MCU firmware wrong Toyota CAN IDs | 3rd | CAN capture shows SWC frames at expected IDs; MCU Monitor still shows nothing | Reflash STM32 with correct Limcet firmware |
| Key code mapping broken in ARK1668 app | 4th | MCU Monitor shows bytes changing on button press; no UI action results | Run FK Learn or investigate libMcuCenter.so key handler |
| CAN bus speed mismatch | check alongside 3rd | Capture shows garbled/no frames | Verify MCU firmware baud rate = 500 kbit/s |

**Note on FK Learn:** The original Prado userdata had no FK Learn entries in `carsetting.ini`
yet SWC worked — confirming the key mapping is hard-coded in libMcuCenter.so, not learned.
FK Learn is NOT the primary fix path.
