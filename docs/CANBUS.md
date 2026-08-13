# Canbus

**Status:** Reference
**Last Updated:** 2026-07-15

## Overview

See also [`REAR_DVD_CANBUS_INVESTIGATION.md`](REAR_DVD_CANBUS_INVESTIGATION.md) — an open
investigation into controlling the factory rear DVD/RSE unit from the Limcet box via CAN,
built on the SWC/MCU findings below.

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

### Reference: Toyota CAN DBC

A useful reference for Toyota CAN message/signal definitions (steering angle, gear,
lights, wheel speeds, SWC, etc.) is comma.ai's opendbc database:

- [`toyota_2017_ref_pt.dbc`](https://github.com/commaai/opendbc/blob/master/opendbc/dbc/toyota_2017_ref_pt.dbc)
  — Toyota reference *powertrain* bus (191 messages, original Toyota signal names).

Caveats: it's a **2017 US** reference on the **powertrain/ADAS** bus — IDs vary by
model year/market, and body/AVN signals (SWC, illumination) may sit on a different
bus than the one the Prado's MCU is wired to. Treat its IDs as candidates to
confirm via live capture. (Note: `0x25` in that DBC is the **steering-angle
sensor**, not SWC.)

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

`CanType=0` means the ARK1668 application does not run one of `libCanBus.so`'s own
vendor CAN-decoder adapters (see below) — for this hardware, CAN decoding is done
entirely by the STM32 MCU firmware and handed to the app as pre-decoded key codes
over `/dev/ttyHS0` via `McuType=6`. This is correct for this hardware. Do not change
`CanType` unless switching to a different MCU adapter that handles CAN at the
application layer (e.g. `McuAdapter_BoxP230` for Honda XBS), or to a `libCanBus.so`
adapter for an external CAN decoder box (see below) — this device has neither.

`McuType=6` selects the Limcet protocol adapter in `libMcuCenter.so`, which matches
the `Limcet-V1.0-1302` MCU firmware.

A `CanSubType` key also exists (referenced as a string in the app binary) but is not
set in any config file present in this rootfs dump — it's presumably a finer-grained
selector used by other products in this OEM's lineup that do enable a `libCanBus.so`
adapter. Not relevant to this device as currently configured.

---

## Known CAN-capable MCU adapters (libMcuCenter.so)

Within `libMcuCenter.so` specifically, only one software adapter has CAN bus methods:

| Adapter | CAN methods | Vehicle |
|---------|-------------|---------|
| `McuAdapter_BoxP230` | `makeCanBusProtocol`, `sendCanBusKeyData`, `writeCanBusData`, `recvCanDatas`, `processSWCKey` | Honda XBS |

This adapter is Honda-specific. For the Prado (McuType=6 / Limcet), CAN decoding is
handled entirely within the STM32 MCU firmware — the ARK1668 software receives only
the decoded key codes. (`libCanBus.so` is a separate library with its own,
much larger set of CAN adapters — see next section.)

---

## `libCanBus.so` — a separate, generic multi-vendor CAN adapter SDK

`libCanBus.so` is a **different library from `libMcuCenter.so`**, and turns out to be
a generic aftermarket CAN-decoder-box SDK shared across this OEM's product line, not
code written specifically for the Prado/Limcet unit. It selects an implementation at
runtime through a factory function:

```
CanBusAdapter::getAdapterInstance(CanBusType)
CanBusAdapter::getCanBusType()
```

The concrete adapter classes found by string search (one `.so`, ~787 KB in the
restored Prado build):

| Class | Likely vehicle / adapter box |
|-------|-------------------------------|
| `CanBus_Raise_Toyota` | Toyota — "Raise"-brand aftermarket CAN box |
| `CanBus_Raise_Honda` | Honda |
| `CanBus_Raise_Nissan` | Nissan |
| `CanBus_Raise_GM` | GM |
| `CanBus_Raise_Haval` | Haval |
| `CanBus_Raise_GAC` | GAC |
| `CanBus_Raise_Venucia` | Venucia |
| `CanBus_Raise_Renault` | Renault |
| `CanBus_Raise_Jeep` | Jeep |
| `CanBus_Raise_Volkswagen` | Volkswagen |
| `CanBus_XBS_Mazda` | Mazda — "XBS"-brand box |
| `CanBus_XinHang` | XinHang-brand box |
| `CanBus_XinRi` | XinRi-brand box |
| `CanBus_LiHang_JMCE200N` | LiHang JMCE200N box |
| `CanBus_Huida_ZD` | Huida ZD box |
| `CanBus_DaoJun_Honda` | DaoJun-brand Honda box |

`CanBus_Raise_Toyota` — the vehicle-correct entry for this Prado — is present, which
at first looks like it should be the active path instead of the MCU. Two pieces of
evidence point the other way, i.e. that it's unused on this device:

1. **Same UART, not a separate CAN peripheral.** `libCanBus.so`'s own strings
   (`CANPortName`, `"Open CanBus Serial Port "` immediately followed by
   `/dev/ttyHS0` in the string table) show it also talks over `/dev/ttyHS0` — the
   same port the MCU (`libMcuCenter.so`) uses. `libCanBus.so`'s "Raise/XBS/XinHang/…"
   classes are written to drive an **external, UART-connected aftermarket CAN decoder
   box** as an alternative to this OEM's own STM32-based Limcet MCU, not to talk to a
   SoC CAN peripheral directly (the ARK1668 has none) or to the onboard TJA1042 (that
   transceiver is wired to the STM32's own bxCAN pins, not to the Linux side at all —
   see Hardware above).
2. **`CanType=0`, not a `CanBus_Raise_*` enum value.** `CanType=0` on this device is
   consistent with "no `libCanBus.so` adapter active" — decoding is left entirely to
   the Limcet MCU firmware via `McuType=6` instead.

**`CanType` → adapter class, full table (disassembly-confirmed 2026-08-03).**
`CanBusAdapter::getAdapterInstance(CanBusType)` (`libCanBus.so`, `0x23d40`) is a
classic ARM jump table: `sub r3, r5, #1; cmp r3, #15; addls pc, pc, r3, lsl #2` —
table index is `CanType − 1`, valid for `CanType` `1`–`16`; `0` or anything outside
that range falls through to a no-op path (no adapter constructed, matches this
device's `CanType=0`). Spot-checked directly against the disassembly (not just the
class names) for `1`, `9`, and `11`:

| `CanType` | Adapter class constructed |
|:--:|---|
| 0 | *(none — no adapter, current value on this device)* |
| 1 | `CanBus_LiHang_JMCE200N` |
| 2 | `CanBus_Huida_ZD` |
| 3 | `CanBus_Raise_Volkswagen` |
| 4 | `CanBus_XinHang` |
| 5 | `CanBus_XBS_Mazda` |
| 6 | `CanBus_XinRi` |
| 7 | `CanBus_Raise_Honda` |
| 8 | `CanBus_Raise_Nissan` |
| **9** | **`CanBus_Raise_Toyota`** |
| 10 | `CanBus_Raise_GM` |
| 11 | `CanBus_Raise_Haval` |
| 12 | `CanBus_Raise_GAC` |
| 13 | `CanBus_Raise_Venucia` |
| 14 | `CanBus_Raise_Renault` |
| 15 | `CanBus_Raise_Jeep` |
| 16 | `CanBus_OdieBenz` |

**Explains the user's live finding (2026-08-03): `CanType=1` breaks all touch/knob
input.** It doesn't fail benignly — it actively constructs `CanBus_LiHang_JMCE200N`,
a real adapter for an entirely different vendor's CAN decoder box (a climate/HVAC
-control-capable adapter, per its own method names like `setAirVolume`/
`setTemperature`/`setWindDirect` — clearly not a Toyota part), which opens
`/dev/ttyHS0` in a mode that fights the Limcet MCU's own use of that same port. The
device's real Toyota-specific class, `CanBus_Raise_Toyota`, is `CanType=9` — not
currently used on this device (CAN decoding is left to the external STM32 MCU via
`McuType=6` instead, i.e. `CanType=0` is correct for this hardware as shipped).
**Do not set `CanType` to anything other than `0` here.**

This reframes (rather than contradicts) the "Root cause" findings below: the
`CanBusKeyManager` / `CanBusKey.config` machinery that Holden's firmware stripped
appears to be a **generic key-code mapping layer** that both paths (MCU-relayed keys
*and* a `libCanBus.so` adapter's decoded keys) can feed into — its presence doesn't
imply a `libCanBus.so` adapter is actively running for this Prado configuration.

One adapter, `CanBus_XinHang`, has its own `onStartUpdateMCU` method and references
a `can_xinhang.bin` file — some of these adapter boxes are themselves updatable from
the Linux side over UART. Not relevant to the Limcet STM32 firmware update path
described above, since `CanType=0` means this class is never instantiated here.

**`gpio34`** — found in `libCanBus.so` as literal shell commands
(`echo 0/1 > /sys/class/gpio/gpio34/value`) — is plausibly a power/enable or
presence-detect line for one of these *external* CAN adapter boxes, not the onboard
TJA1042 (see point 1 above; that transceiver is enabled/controlled by the STM32, not
by a Linux GPIO). This is inference — no string ties `gpio34` to a specific purpose,
and it is presumably unused/floating on this device since `CanType=0`. See
[`hardware/BOARD_ANALYSIS.md`](../hardware/BOARD_ANALYSIS.md) for the full GPIO table.

**Live hardware test, 2026-08-03: `CanType=1` breaks all touch/knob input.** User changed `MsnProductInfo.ini`'s `CanType` from `0` to `1` (exploring undocumented settings) and touch/knob input stopped responding entirely. Not yet disassembled to find the exact enum mapping, but this is consistent with, and indirectly confirms, point 2 above: `CanType=1` most likely instantiates a real `CanBus_Raise_*` adapter class expecting an *external* UART-connected CAN decoder box on `/dev/ttyHS0` -- the same port the Limcet MCU uses for its own touch/knob/key relay. This hardware has no such external box (onboard TJA1042 is STM32-side only, not Linux-visible -- see point 1 above), so a `libCanBus.so` adapter either steals the port from the MCU or simply never receives real frames -- either way, the generic `CanBusKeyManager` key-mapping layer both touch and knob input feed through never gets fed. **Do not set `CanType` to anything other than `0` on this hardware.**

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