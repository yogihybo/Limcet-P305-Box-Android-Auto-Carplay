# MCU Adapter Types — libMcuCenter.so (ARK1680 / Limcet-P306)

Reverse-engineered from `rootfs/usr/lib/libMcuCenter.so` symbol table.

The MCU adapter is selected at runtime by `McuType` in `MsnProductInfo.ini`.
`MCUAdapter::getAdapterInstance(McuType)` instantiates the correct subclass,
which manages the serial protocol to the external MCU on `MCUPortName`
(default `/dev/ttyHS0` for the P300 family).

---

## Current Prado configuration

```ini
McuType=6
MCUPortName="/dev/ttyHS0"
CanType=0
```

McuType=6 selects one of the adapters below. Based on the product numbering and
the P30x family, McuType=6 most likely maps to **MCUAdapter_BoxP300** or
**MCUAdapter_BoxP700** (see notes under each).

---

## Adapter Catalogue

### MCUAdapter_BoxP100
**Type:** Basic dual-framing (two independent packet parsers)  
**Features:** `sendData`, `sendPhoneConnectState`, `sendReadyPackage`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Notes:** Simple protocol with two frame sizes (`onRecvData2`, `getPackageMinSize2`). Likely a legacy or cut-down MCU.

---

### MCUAdapter_BoxP200
**Type:** MCU-updatable, mode-switching  
**Features:** `msnAppStateChange`, `onModeAppChange`, `sendPhoneConnectState`, `sendYModemDatas`  
**SWC:** None  
**CAN:** None  
**Update:** Yes — YModem firmware upload (`tryOpenUpdateFile`, `sendUpdateFileInfo`, `sendUpdateFileData`)  
**Notes:** P200 has full disk-status callbacks and firmware OTA. One of the more capable base types.

---

### MCUAdapter_BoxP210
**Type:** Settings panel + mode switching  
**Features:** `makeMCUProtocol`, `syncSettingDataToMcu`, `showSelectDialog`, `getSetItemValueTexts`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Notes:** Has a car-setting UI (SetItems) synced to MCU. Likely a head unit with physical control knobs.

---

### MCUAdapter_BoxP220
**Type:** Minimal protocol  
**Features:** `makeMcuProtocol`, `showApp`, `onRecvMcuProtocol`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Notes:** No `getPortSettings` override (inherits default). Very simple adapter.

---

### McuAdapter_BoxP230  ⭐ CAN bus SWC adapter
**Type:** Honda XBS (cross-brand switcher) — CAN bus steering wheel  
**Features:**  
- `processSWCKey` — parses raw SWC ADC/CAN key strings from MCU  
- `makeCanBusProtocol` / `sendCanBusKeyData` / `writeCanBusData` / `recvCanDatas` — full CAN frame handling  
- `onModeSelectChange` / `showModeSelectDialg` — mode selector popup  
- `onKeyEvent` — decoded key events  
- `onSwitchReversingView` — camera switching  
- `switchSpeeker` — audio routing  
- `syncSettingData` / `writeSettingData` — MCU settings sync  

**SWC:** YES — both ADC (`processSWCKey`) and CAN bus path  
**CAN:** YES — bidirectional CAN frame encode/decode  
**Update:** None  
**Resources:** `%1resources/Box-P230.rcc` (contains `xbs_honda/` Honda-specific UI)  
**Notes:** The **only** adapter with CAN bus SWC support. Designed for Honda vehicles with
CAN-connected steering wheel buttons. If the Prado SWC uses CAN, this is the adapter
that would need to be active (or a Toyota equivalent added). Note the lowercase `c` in
`McuAdapter` vs all others being `MCUAdapter`.

---

### MCUAdapter_BoxP300
**Type:** P3xx product family — settings panel + firmware update  
**Features:** `onModeAppChanged`, `makeMCUProtocol`, `syncSettingDataToMcu`, `showSelectDialog`  
**SWC:** None  
**CAN:** None  
**Update:** Yes — `onStartUpdateMCU`, `onSendUpdateReadyTimer`, `onDiskStatusChange`  
**Notes:** The P300-series adapter. Used by Limcet-P306 (McuType likely 5 or 6).
No steering wheel ADC or CAN support. MCU sends pre-decoded key events via
the serial protocol.

---

### MCUAdapter_BoxP400
**Type:** Compact mode switcher  
**Features:** `makeMcuProtocol`, `onModeAppChange`, `translateApp`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Notes:** Minimal feature set. Likely a cost-reduced MCU variant.

---

### MCUAdapter_BoxP500
**Type:** Dual-framing + firmware update + launcher  
**Features:** `sendData2`, `sendPhoneConnectState`, `onModeAppChange`, `onLauncherVisibleChange`, `showUpdateDialog`  
**SWC:** None  
**CAN:** None  
**Update:** Yes (`showUpdateDialog`)  
**Notes:** Two frame formats like BoxP100 but with firmware update. Launcher visibility callback.

---

### MCUAdapter_BoxP700
**Type:** LED + long-press  
**Features:** `onLongPressTimeout`, `onLEDTimer`, `makeMcuProtocol`, `translateApp`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Resources:** `Box-P700`, `Box-P702` (named in strings)  
**Notes:** Has LED control and long-press timer — suggests a physical button panel with illumination.

---

### MCUAdapter_BoxP701
**Type:** P701 — firmware-updatable variant of P700  
**Features:** `makeMcuProtocol`, `writeReplayPackage`, `onLongPressTimeout`, `onStartUpdateMCU`, `onSendUpdateDatasToMCU`  
**SWC:** None  
**CAN:** None  
**Update:** Yes — dedicated update protocol  
**Notes:** P701 adds firmware update and replay-package capability over P700.

---

### MCUAdapter_BoxP800
**Type:** Multi-camera / FM transmitter settings panel  
**Features:** `makeMcuProtocol`, `onSettingChange`, `retrySendStartupStatus`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Settings UI:** `BoxP800SettingWindow` — tabs for camera type selection (OE/AfterMarket/360),
FM transmitter frequency slider, right camera, reverse track, radar toggles  
**Config file:** `SettingBoxP800.config`  
**Notes:** The most feature-rich camera/AV settings UI. Has FM transmitter tuning.

---

### MCUAdapter_BoxP900
**Type:** P900 — settings + firmware update with retry  
**Features:** `makeMcuProtocol`, `onLongPressTimeout`, `syncSettingDataToMcu`, `onStartUpdateMCU`, `retryOpenUpdateFile`  
**SWC:** None  
**CAN:** None  
**Update:** Yes — with retry logic and `getUpdateFilePath`  
**Notes:** Uses `SetItemTypes` enum (typed rather than indexed SetItems). More robust update path.

---

### MCUAdapter_BoxC230
**Type:** C-series compact (no settings panel)  
**Features:** `makeMcuProtocol`, `onLongPressTimeout`  
**SWC:** None  
**CAN:** None  
**Update:** None  
**Notes:** Minimal adapter. `C` prefix likely denotes a different MCU hardware variant or connector.

---

### MCUAdapter_BoxC250
**Type:** C-series with heartbeat + dual-framing  
**Features:** `onHeartbeatTimer`, `sendPhoneConnectState`, `sendData2`, `showUpdateDialog`  
**SWC:** None  
**CAN:** None  
**Update:** Yes  
**Notes:** Heartbeat ping-pong with MCU. Dual frame formats.

---

### MCUAdapter_BoxC270
**Type:** C-series — settings panel + display switch + firmware update  
**Features:** `switchDisplay`, `onLongPressTimer`, `makeMCUProtocol`, `syncSettingDataToMcu`, `onStartUpdateMCU`  
**SWC:** None  
**CAN:** None  
**Update:** Yes  
**Notes:** Has `switchDisplay` — likely handles a dual-display or AV-switch setup.

---

### MCUAdapter_CarA200
**Type:** Legacy / minimal car adapter  
**Features:** `onRecvMcuProtocol`, `onRecvAppProtocol` only  
**SWC:** None  
**CAN:** None  
**Notes:** Stub adapter — overrides only the two protocol receive handlers. Possibly for a
very simple car-specific MCU.

---

### MCUAdapter_CarA300
**Type:** Car adapter with OTA update and ready-packet handshake  
**Features:** `initAdapter`, `onStartUpdateMCU`, `sendReadyData`, `onSendReadyTimer`, `onDiskStatusChange_super`  
**SWC:** None  
**CAN:** None  
**Update:** Yes  
**Notes:** Has `initAdapter` override (most adapters inherit base init). `_super` suffix on
`onDiskStatusChange` suggests it calls the base class implementation.

---

### MCUAdapter_CarA301
**Type:** Minimal car adapter  
**Features:** `getPortSettings`, `onInited`  
**SWC:** None  
**CAN:** None  
**Notes:** Only overrides port settings and init — everything else inherited. A301 is likely
a minor protocol variant of CarA300.

---

### MCUAdapter_HUD
**Type:** Head-up display controller  
**Features:** `setBackLightValue`, `onAutoBacklightTimer`, `filterKey`, `isCallState`, `msnAppNotify`  
**SWC:** None  
**CAN:** None  
**Notes:** Backlight control with auto-dimming. `filterKey` suggests it intercepts certain
key events for HUD-specific handling. `isCallState` integrates with BT call status.

---

### MCUAdapter_Bagoo
**Type:** Bagoo-brand MCU  
**Features:** `onKeyEvent`, `onLongPressTime`  
**SWC:** None (receives pre-decoded key events)  
**CAN:** None  
**Notes:** `onKeyEvent(uint,bool,bool)` = keyCode, isPress, isAutoRepeat. Bagoo is a Chinese
MCU board manufacturer; this adapter handles their specific serial protocol.

---

### MCUAdapter_NV17
**Type:** NV17 MCU board  
**Features:** `onKeyEvent`  
**SWC:** None (receives pre-decoded key events)  
**CAN:** None  
**Notes:** Very similar to Bagoo. `onKeyEvent` receives key code, press/release, and auto-repeat flags.

---

### MCUAdapter_IM60BC
**Type:** IM60BC MCU — speaker switching + key events  
**Features:** `switchSpeeker`, `switchSpeeker2`, `onKeyEvent`, `msnAppNotify`, `onModeAppChange`, `syncSettingDataToMcu`  
**SWC:** None (pre-decoded key events via `onKeyEvent`)  
**CAN:** None  
**Notes:** Two speaker-switch calls suggests it controls front/rear or main/sub amplifier relay.
Has a settings panel (SetItems). `msnAppNotify` handles events from the main app.

---

### MCUAdapter_MsnDecoder
**Type:** MSN decoder — DVR/camera multi-channel  
**Features:** `getDVRViewChannle`, `msnAppNotify`, `syncAllSettingDatasToMcu`, `onModeAppChange`  
**SWC:** None  
**CAN:** None  
**Notes:** Has `getDVRViewChannle` — handles a multi-channel DVR or camera matrix.
`syncAllSettingDatasToMcu` bulk-syncs all settings at once.

---

## Summary table

| Adapter | SWC | CAN | Update | Settings | Key Events | Notes |
|---------|-----|-----|--------|----------|------------|-------|
| BoxP100 | — | — | — | — | Raw protocol | Legacy dual-frame |
| BoxP200 | — | — | YModem | — | — | OTA updatable |
| BoxP210 | — | — | — | SetItems | — | Physical knob panel |
| BoxP220 | — | — | — | — | — | Minimal |
| **BoxP230** | **YES** | **YES** | — | SetItems | onKeyEvent | Honda XBS, only CAN SWC |
| BoxP300 | — | — | Yes | SetItems | — | P3xx family (current Prado) |
| BoxP400 | — | — | — | — | — | Compact |
| BoxP500 | — | — | Yes | — | — | Dual-frame + OTA |
| BoxP700 | — | — | — | — | — | LED + long-press |
| BoxP701 | — | — | Yes | — | — | P700 + OTA |
| BoxP800 | — | — | — | Camera/FM | — | Camera matrix + FM TX |
| BoxP900 | — | — | Yes | SetItemTypes | — | Typed settings |
| BoxC230 | — | — | — | — | — | C-series minimal |
| BoxC250 | — | — | Yes | — | — | C-series + heartbeat |
| BoxC270 | — | — | Yes | SetItems | — | C-series + display switch |
| CarA200 | — | — | — | — | — | Legacy stub |
| CarA300 | — | — | Yes | — | — | Car OTA |
| CarA301 | — | — | — | — | — | Minimal car |
| HUD | — | — | — | — | filterKey | Heads-up display |
| Bagoo | — | — | — | — | onKeyEvent | Bagoo MCU board |
| NV17 | — | — | — | — | onKeyEvent | NV17 MCU board |
| IM60BC | — | — | — | SetItems | onKeyEvent | Speaker switch |
| MsnDecoder | — | — | — | SetItems | — | DVR/camera matrix |

---

## Version screen baseline (from working device)

Captured from the device when it was working, via Settings → About:

| Field | Value |
|-------|-------|
| BT | BT825, V5.5.0 |
| Software | Limcet-P306 V3.10.3.0212 |
| Hardware | 02-0006-06-00-00-00-00-00 |
| System | Limcet 2022-02-12 |
| MCU | Limcet-V1.0-1302 |

- **BT825, V5.5.0** — FSC-BT8251 Feasycom module, firmware V5.5.0 (matches `blueware.properties`)
- **Software V3.10.3.0212** — the original Limcet-P306 application; the Holden base firmware is a different version
- **System 2022-02-12** — rootfs build date
- **MCU Limcet-V1.0-1302** — the Limcet-branded MCU firmware on the physical STM32/STM8 chip
- **Hardware 02-0006-06-00-00-00-00-00** — board hardware revision string

The MCU firmware is stored on the external MCU chip (not in the ARK1668 NAND). No MCU
update binaries are present in the Holden rootfs, so flashing a new rootfs does NOT
overwrite the MCU firmware. The MCU chip should still contain `Limcet-V1.0-1302`.

---

## MCU role — touch AND key events

The Limcet MCU is an **STM32F105RBT6** (ARM Cortex-M3, 72MHz, 128KB Flash, LQFP64)
on the DC_LIMCET_MB_REV_003 board. It handles:
- **Touchscreen input** — the advanced factory menu MCU Monitor shows raw touch events
- **Steering wheel buttons** — ADC voltage divider from SWC input wire
- **Panel buttons** — physical buttons on the head unit bezel
- **ACC/IGN detection** — power management
- **Reverse signal** — triggers camera view

All events arrive at the ARK1668 via `/dev/ttyHS0` using the Limcet protocol (`McuType=6`).
Because touchscreen works, the MCU↔ARK1668 UART link is confirmed functional.

---

## McuType dropdown values (from libSetting.so factory menu)

The factory settings menu McuType dropdown label list (0-indexed):

| Index | Label | MCU chip |
|-------|-------|----------|
| 0 | msn_stm32 | STM32 generic |
| 1 | msn_stm8 | STM8 generic |
| 2 | msn_stm8_9600 | STM8 at 9600 baud |
| 3 | LingFei | LingFei OEM |
| 4 | Limcet | Limcet STM MCU |
| 5 | CheKuShiDai | |
| 6 | ZongLian | |
| 7 | LanMo | |
| 8 | ZhiYunLianChe | |
| 9 | NV17 | → MCUAdapter_NV17 |
| 10 | Bagoo | → MCUAdapter_Bagoo |
| 11 | IMBC60 | → MCUAdapter_IM60BC |
| 12 | ChangDao | |
| 13 | msn_box | |
| 14 | ziqi | |
| 15 | che yuan yin | |
| 16 | nfck | |
| 17 | msn_hud | → MCUAdapter_HUD |
| 18 | FOSP | |
| 19 | XBS_32PIN | |
| 20 | msn_nordic | Nordic MCU |

`McuType=6` in MsnProductInfo.ini is the integer index. Based on the device
reporting "MCU: Limcet-V1.0-1302" when working, and the label list, the Prado
device uses the **Limcet** adapter (index 4) or a nearby variant. The exact index
mapping to `MCUAdapter_BoxPxxx` classes in `libMcuCenter.so` requires disassembly
of the `getAdapterInstance()` switch statement to confirm.

---

## Prado SWC — ADC key learning (not CAN bus)

The STM32F105RBT6 MCU reads an ADC voltage divider on the SWC input wire from the
steering wheel harness. Different buttons produce different resistance values → different
voltages. The 12-bit ADC (16 channels) maps these to key codes sent to the ARK1668
via `/dev/ttyHS0`.

**CAN bus possibility:** The STM32F105 also has 2× hardware bxCAN controllers. If a
CAN transceiver IC (TJA1050 or similar) is populated on the board near the MCU, the
MCU firmware could read Toyota Prado SWC button messages directly from the CAN bus.
Worth checking the board — if a small SOIC-8 chip is connected to the MCU's CAN pins
(PA11/PA12 or PB8/PB9), CAN is wired up.

Because touch events already work (proving MCU↔ARK comms is active), the SWC failure
is a **key mapping / learning issue**, not a hardware or protocol problem.

### SWC Learning feature (found in libSetting.so)

The firmware has a full SWC key-learning UI:
- `FKLearnWindow` — "Long press the steering wheel key to complete the setup"
- `EnableSWCSwitchHardware` — FactoryConfig flag that activates the SWC hardware path
- `EnableFKLearn` — FactoryConfig flag that shows the FK Learn button in Settings → About

**Both flags are now added to `msn_factory_configs/FactoryConfig.ini`.**

### How to learn the steering wheel keys

1. Flash the updated userdata (with new FactoryConfig) or edit via SSH
2. Go to **Settings → System Info** (the About screen)
3. The "FK Learn" button should now be visible
4. Press each steering wheel button and long-press to teach it
5. The learned mapping is saved to userdata

### If SWC Learning screen does not appear

The hidden factory menu items (1-6) may include additional SWC options. Temporarily
set in FactoryConfig:
```ini
DisableFactorySetItems=""
```
This exposes the full factory menu. Look for "SWC Learning" or "Panel Key Learning"
entries. Re-hide after use.

### MCU serial port
The MCU communicates with the ARK1668 via `/dev/ttyHS0`. To monitor what key events
the MCU is actually sending, enable MCU debug logging:

```sh
# Via SSH or serial console
touch /data/mcudebug_flag
# Then check logcat or /tmp for MCU key event output
```

The file `/data/mcudebug_flag` enables debug mode in `libMcuCenter.so` (referenced in
the binary as the debug flag path).
