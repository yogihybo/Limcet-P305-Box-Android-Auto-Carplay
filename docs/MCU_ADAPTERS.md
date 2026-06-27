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

## Prado SWC — CAN bus analysis

The Toyota Prado 150 series uses the **CAN bus** for steering wheel controls. The factory
head unit reads button presses off the CAN network directly. When replacing with an
aftermarket head unit (Limcet-P306), an external **CAN-to-SWC interface module** is
typically required, such as:

- **Connects2 CTASW002** or similar Toyota-specific CAN SWC adapter
- **iDatalink Maestro** (if available for Prado)
- A generic CAN SWC decoder that outputs analog voltage levels or serial key codes

### Current software state
- `CanType=0` in `MsnProductInfo.ini` — CAN is disabled
- `McuType=6` maps to a P3xx/P7xx adapter — **no CAN SWC support**
- `McuAdapter_BoxP230` is the only adapter with CAN bus methods, but it is Honda-specific

### Options for fixing Prado SWC

**Option A — External CAN-to-analog adapter (recommended)**  
Use a Toyota-specific CAN SWC adapter module between the Prado CAN bus and the head
unit's SWC/REMOTE input pin. The adapter converts CAN button messages to analog voltage
levels. The head unit's MCU reads these as standard ADC SWC input. No firmware changes
needed — the current McuType=6 adapter handles pre-decoded key codes from the MCU.

**Option B — Verify MCU ADC thresholds**  
If an analog adapter is already connected but keys don't register, the MCU firmware may
need reconfiguration. The MCU reads an ADC input and maps voltage ranges to key codes.
The voltage thresholds compiled into the MCU firmware must match the output of the
connected adapter. This requires access to the MCU firmware (separate from the ARK1668 OS).

**Option C — Software key learning**  
The factory menu may have a hidden SWC key learning mode. Items 1-6 are hidden by
`DisableFactorySetItems="1,2,3,4,5,6"` in `FactoryConfig.ini`. Temporarily removing
that line may expose a SWC key-learn screen where each button is taught individually.

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
