# libMcuCenter.so — full structural unpack

Complete structural reverse-engineering of the SoC-side MCU driver
`rootfs/usr/lib/libMcuCenter.so` (ARK1680 / Limcet-P306). This is the companion
reference to [MCU_ADAPTERS.md](MCU_ADAPTERS.md) (adapter catalogue, McuType factory
map, BoxP300 command dispatch, live-capture guide) and
[../MCU/MCU_FIRMWARE_REVIEW.md](../MCU/MCU_FIRMWARE_REVIEW.md) (the STM32 side).

> **Scope / honesty.** "Full unpack" here = the complete **structure**: every class
> and its methods, the class hierarchy, the wire protocol (frame format + checksum,
> disassembly-verified), and the config/string surface. It is **not** a line-by-line
> decompilation of all 978 functions — most are Qt/UI boilerplate. Behaviour that is
> disassembly-verified is marked; the rest is symbol-level structure.

---

## 1. Binary overview

| Property | Value |
|---|---|
| Format | ELF32, ARM (EABI), shared object |
| `.text` | 470,572 B |
| Defined functions | 978 |
| Classes (typeinfo) | 39 (48 incl. `Ui_*`/support) |
| Toolkit | Qt 4 (`QtGui`, `QtCore`, `QtNetwork`) |
| Key deps | `libMsnCommons.so`, `libQtGui/Core/Network.so.4`, `libts-0.0` (touch), `libpng15`, `libjpeg.9`, `libz`, `libstdc++`, `libdl` |
| Entry (plugin) | `McuCenterPlugin::createMainWindow` |

The library is a **plugin** loaded by `MsnCoreApp`; `McuCenterPlugin` is the entry
point, and `MCUAdapter::getAdapterInstance(McuType)` (`0x025e40`) builds the
concrete adapter selected by `MsnProductInfo.ini`.

---

## 2. Wire protocol — frame format (disassembly-verified for the P300 family)

Decoded from `findPackageStartSig` / `getPackageSize` / `getPackageCheckSum`:

```
byte[0]        = 0x2E              start signature ('.')  (findPackageStartSig searches for this)
byte[1]        = command code      dispatch key (see BoxP300 table in MCU_ADAPTERS.md)
byte[2]        = payload length N   (getPackageSize returns N + 4)
byte[3 .. 3+N-1] = payload
byte[3+N]      = checksum
total frame     = N + 4 bytes       (4 = sig + cmd + len + checksum)
```

**Checksum algorithm** (`getPackageCheckSum`, `0x31c18`):
```c
uint8_t sum = 0;
for (i = start; i < start+count; i++) sum += data[i];
checksum = (~sum) & 0xFF;          // one's-complement 8-bit sum
```

Notes:
- The framing is **per-adapter (virtual)**. The above is the P300/BoxP200 family.
  `MsnDecoder` (McuType 16, Holden) uses a **different** layout — min 6 bytes, a
  group byte at offset 2 and command at offset 3 (see MCU_ADAPTERS.md).
- Dual-framing adapters (`BoxP100`, `BoxP500`, `D107`) implement a **second** parser
  (`*2`-suffixed: `findPackageStartSig2`, `getPackageSize2`, …) for two frame types.
- Outbound frames are built by `makeMCUProtocol` / `makeMcuProtocol(Package)` and the
  checksum by `getMcuProtocolCheckSum` / `makeMcuProtocolCheckSum`.

---

## 3. Class architecture

```
QObject
 └─ MCUAdapter                     abstract base + factory (getAdapterInstance)
     │   getMcuType, getPortName/Settings, initAdapter,
     │   onParentRecvMcuProtocol, sendProtocolToCoreApp / sendProtocolToMsgCenter
     │
     ├─ MCUAdapter_BoxP200         "fat" base: full framing + update + settings
     │   (most Box* adapters inherit this machinery)
     │
     ├─ MCUAdapter_BoxP300         ← Prado (McuType 6): overrides framing (sig 0x2E),
     │                               onRecvMcuProtocol dispatch, makeMCUProtocol
     ├─ MCUAdapter_MsnDecoder      ← Holden (McuType 16): different framing, DVR/camera
     ├─ McuAdapter_BoxP230         only CAN/SWC adapter (Honda XBS)
     ├─ MCUAdapter_Box_Encryption  encrypted-link variant (thin)
     ├─ MCUAdapter_HUD             head-up display (backlight, instrument)
     ├─ MCUAdapter_{Bagoo,NV17,IM60BC,ZhongHang,RuiYuanSWC}   pre-decoded key-event MCUs
     ├─ MCUAdapter_{BoxP400..P900, BoxC230..C290, BoxP210/220, D107}   product variants
     └─ MCUAdapter_{CarA200,CarA300,CarA301}   minimal car adapters
```

**The common adapter interface** (virtuals every adapter overrides some of):
- **Framing:** `findPackageStartSig`, `getPackageSize`, `getPackageMinSize`,
  `getPackageCheckSum`, `isAvaliablePackage` [sic].
- **Protocol I/O:** `onRecvMcuProtocol` (inbound dispatch), `onRecvAppProtocol`,
  `makeMCUProtocol`/`makeMcuProtocol`, `sendMcuProtocolData`, `sendReadyPackage`.
- **Settings UI (SetItems):** `getSetItemText`, `getSetItemValueTexts`,
  `getSetItemValueIndex`, `getSetItemDefValueIndex`, `syncSettingDataToMcu`,
  `showSelectDialog`, `onItemListViewClicked`.
- **Firmware update:** `onStartUpdateMCU`, `onDiskStatusChange`, `tryOpenUpdateFile`,
  `checkMCUUpdateFile`, `readyToUpdateMCU`, `sendUpdateFileInfo`/`Data`,
  `sendYModemDatas`, `statrtUpdateMCUFile` [sic].
- **App integration:** `msnAppNotify`, `msnAppStateChange`, `onModeAppChange(d)`,
  `showApp`, `translateApp`.

**Capability by method presence** (reliable — a method exists ⇒ compiled in):
- **CAN/SWC:** only `BoxP230` (`makeCanBusProtocol`, `writeCanBusData`,
  `recvCanDatas`, `processSWCKey`, `sendCanBusKeyData`).
- **Pre-decoded key events (`onKeyEvent`):** `Bagoo`, `NV17`, `IM60BC`, `ZhongHang`,
  `RuiYuanSWC`, `BoxP230`.
- **Firmware update (`onStartUpdateMCU`/YModem):** `BoxP200`, `BoxP300`, `BoxP400..P900`,
  `BoxC270/280/290`, `BoxP701`, `CarA300`.
- **Dual-framing (`*2`):** `BoxP100`, `BoxP500`, `D107`.
- **DVR/camera matrix:** `MsnDecoder` (`getDVRViewChannle`).
- **HUD:** `MCUAdapter_HUD` (`setBackLightValue`, `filterKey`, `isCallState`).

---

## 4. App-event interface (how inbound frames reach the UI)

`onRecvMcuProtocol` handlers construct an **`MsnEvent`** and post it into the Qt app:
- Ctors: `MsnEvent(uint, MsnEventType)` and `MsnEvent(uint, uint, MsnEventType)`.
- Params: `setParams(u64,u64)`, `setVariantParam`, `setStringParams`,
  `setByteArrayParams`, `setTargetApp(uint)`; getters mirror these.
- Delivered via `QCoreApplication::sendEvent`/`postEvent` →
  `MsnApplication::dispatchMsnEvent` → `MsnMainWindow::msnAppNotify`.
- Direct keypress path: `MsnApplication::simulateKey(keyCode, isPress, isAutoRepeat)`.

The concrete `MsnEventType` codes traced for BoxP300 (`0x01→0x1013`, `0x05→0x5018`,
`0x06→0x501A`, `0x12→0x5026`) are listed in MCU_ADAPTERS.md; their human meanings
require the on-device capture in that doc's capture-guide section.

---

## 5. Config / string surface

**Config keys** (read from `MsnProductInfo.ini` / `FactoryConfig.ini`):
`ProductId`, `ScreenType`, `CameraType`, `LauncherName`, `McuType`, `CanType`,
`MCUPortName`, `MCUBaudSpeed`, `MCUUpdateName`, `MSNEryPortName`.

**Ports/paths:** `MCUPortName="/dev/ttyHS0"`, `MSNEryPortName="/dev/ttyS2"`,
`/dev/ttyS1`, `/msnprofile/`, `/tmp/`, `/tmp/mcuupdate/`, `mcuupdate4/`.

**Debug flags:** `/data/mcudebug_flag`, `/data/mcudebug_flag_msn` — gate frame
logging (`recvProtocolData`, `send msn mcu code!`, `recv track:`, …). See the
capture guide in MCU_ADAPTERS.md.

**MCU BT module AT strings** (outbound to the Feasycom chip, handled MCU-side):
covered in [../MCU/MCU_FIRMWARE_REVIEW.md](../MCU/MCU_FIRMWARE_REVIEW.md).

---

## 6. Full class → method inventory

All 48 classes with public/member methods (ctors/dtors omitted; `[sic]` typos are the
vendor's). Ordered by method count.

_MCU adapters:_

- **MCUAdapter_BoxP200 (41)** — the fat base: `checkMCUUpdateFile, findPackageStartSig, getMcuProtocolCheckSum, getPackageCheckSum, getPackageMinSize, getPackageSize, getPortSettings, getSetItemDefValueIndex, getSetItemText, getSetItemValueIndex, getSetItemValueTexts, isAvaliablePackage, makeMcuProtocolPackage, msnAppStateChange, onAcceptUpdate, onDiskStatusChange, onInited, onItemListViewClicked, onModeAppChange, onReadDeviceDatas, onRecvAppProtocol, onRecvMcuProtocol, onRejectUpdate, readyToUpdateMCU, resetListTexts, sendMcuProtocolData, sendPhoneConnectState, sendReadyPackage, sendUpdateFileData, sendUpdateFileInfo, sendYModemDatas, showApp, showSelectDialog, statrtUpdateMCUFile, syncSettingDataToMcu, translateApp, tryOpenUpdateFile`
- **McuAdapter_BoxP230 (40)** — CAN/SWC (Honda): `findPackageStartSig, getPackageCheckSum, getPackageMinSize, getPackageSize, getPortSettings, getSetItem*, isAvaliablePackage, makeCanBusProtocol, msnAppNotify, onKeyEvent, onModeAppChanged, onModeSelectChange, onRecvAppProtocol, onRecvMcuProtocol, onSendReadyTimer, onSwitchReversingView, processSWCKey, recvCanDatas, sendCanBusKeyData, showModeSelectDialg, switchSpeeker, syncData, syncSettingData, writeCanBusData, writeSettingData`
- **MCUAdapter_BoxP300 (29)** — Prado: `findPackageStartSig, getPackageCheckSum, getPackageMinSize, getPackageSize, getPortSettings, getSetItem*, isAvaliablePackage, makeMCUProtocol, msnAppStateChange, onDiskStatusChange, onInited, onItemListViewClicked, onModeAppChanged, onRecvAppProtocol, onRecvMcuProtocol, onSendUpdateReadyTimer, onStartUpdateMCU, resetListTexts, showApp, showSelectDialog, syncSettingDataToMcu, translateApp`
- **MCUAdapter_MsnDecoder (24)** — Holden: `getDVRViewChannle, getPortSettings, getSetItem*, msnAppNotify, onDiskStatusChange, onModeAppChange, onRecvAppProtocol, onRecvMcuProtocol, showApp, showSelectDialog, syncAllSettingDatasToMcu, syncSettingDataToMcu, translateApp`
- **MCUAdapter_BoxC270 (31)**, **BoxP900 (31)** (`getUpdateFilePath, retryOpenUpdateFile, updateFileEnd`), **BoxC280 (29)**, **BoxP210 (25)**, **BoxP701 (25)** (`onSendUpdateDatasToMCU, writeReplayPackage`), **BoxC290 (21)**, **BoxP800 (21)** (`onSettingChange, retrySendStartupStatus`), **BoxP400 (20)**, **BoxP700 (20)** (`onLEDTimer, onLongPressTimeout`), **BoxC250 (19)** (`onHeartbeatTimer`), **BoxC230 (18)**, **BoxP220 (12)**.
- **Dual-framing:** **D107 (29)**, **BoxP500 (27)**, **BoxP100 (18)** — each with `*2` parser methods + `sendData`/`sendData2`/`showUpdateDialog`.
- **Key-event MCUs:** **IM60BC (28)** (`switchSpeeker/2`), **ZhongHang (17)** (`onLEDTimeout`), **Bagoo (16)**, **NV17 (14)**, **RuiYuanSWC (20)** (`onKeyPressTimeout`).
- **HUD (16)** — `filterKey, isCallState, onAutoBacklightTimer, setBackLightValue`.
- **CarA300 (13)** — `initAdapter, onDiskStatusChange_super, onSendReadyTimer, sendReadyData`; **CarA301 (6)**; **CarA200 (2)**.
- **Box_Encryption (3)** — `getPortName, getPortSettings, onInited` (thin; encrypted link).
- **MCUAdapter (20, base)** — `getAdapterInstance, getMcuType, getPortName, getPortSettings, initAdapter, isInited, msnAppNotify, msnAppStateChange, onParentRecvMcuProtocol, sendProtocolToCoreApp, sendProtocolToMsgCenter, setCaptureEnable, showApp, timerEvent, translateApp`.

_UI / support classes:_

- **HondaRadioWindow (31)** — Honda radio UI (`on_btnAM/FM/AUX/Navi/Num1-6…`).
- **BoxP800SettingWindow (24)** — camera/FM-transmitter settings panel.
- **HUDInstrumentWindow (15)** — `drawEngineSpeedWidget, drawFuel, drawVehicleSpeedWidget, paintEvent`.
- **ModeSelectDialog (14)** — source/mode selector popup.
- **MsnKnobButton (11)** — rotary knob widget (`rotateStep, setRotatePixmap`).
- **McuCenterPlugin (2)** — `createMainWindow, customEvent` (plugin entry).
- **OptionListDelegate (4)**, **Ui_BoxP800SettingWindow / Ui_HondaRadioWindow / Ui_ModeSelectDialog (setupUi)**, and imported-type stubs (`MsnEvent`, `ProtocolUtils`, `QByteArray`, `QString`, `QDebug`, `TableViewItemDatas`, `TableViewModel`).

---

## 7. Open items (need on-device capture — see MCU_ADAPTERS.md capture guide)

- Human meaning of the `MsnEventType` codes (`0x5018`/`0x501A`/`0x5026`).
- The **outbound** command byte set (`makeMCUProtocol` callers).
- Exact MCU link baud (adapter default; not in config — try 115200 then 38400).
