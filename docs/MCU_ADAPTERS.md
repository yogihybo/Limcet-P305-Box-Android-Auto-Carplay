# MCU Adapter Types — libMcuCenter.so (ARK1680 / Limcet-P306)

Reverse-engineered from `rootfs/usr/lib/libMcuCenter.so`.

The MCU adapter is selected at runtime by `McuType` in `MsnProductInfo.ini`.
`MCUAdapter::getAdapterInstance(McuType)` instantiates the correct subclass,
which manages the serial protocol to the external MCU on `MCUPortName`
(default `/dev/ttyHS0` for the P300 family).

> **Evidence level — read before trusting a claim here.**
> - **Disassembly-verified** (high confidence): the `McuType → adapter` factory
>   map (`getAdapterInstance` jump table), `McuType=6 → BoxP300`, the Prado-vs-Holden
>   config diff, and the `BoxP300`/`MsnDecoder` **frame layout + command dispatch**
>   traced from `onRecvMcuProtocol`.
> - **Symbol-table inference** (medium confidence): the per-adapter **Catalogue**
>   below. The `SWC` / `CAN` / `Update` flags come from whether a class *defines*
>   the relevant method (e.g. `writeCanBusData` ⇒ CAN capability compiled in) —
>   this proves the capability exists in the binary, **not** that it is wired up or
>   used at runtime. The prose "Type"/"Notes" are interpretation of method names
>   and have **not** each been confirmed by disassembling the adapter's code.
>
> If a Catalogue conclusion is load-bearing for a decision, disassemble that
> adapter's `onRecvMcuProtocol` / relevant method before relying on it.

---

## Current Prado configuration

```ini
McuType=6
MCUPortName="/dev/ttyHS0"
CanType=0
```

**`McuType=6` → `MCUAdapter_BoxP300` — CONFIRMED** by reversing the factory
`MCUAdapter::getAdapterInstance(McuType)` at `0x025e40`, not guessed. The dispatch
is a jump table `sub r3, McuType, #1; cmp r3, #0x1d; addls pc, pc, r3, lsl #2`, so
**`McuType=0` = none, and 1–30 map to the table below** (index = `McuType − 1`).

### Full McuType → adapter map (from the factory jump table)

| McuType | Adapter | McuType | Adapter |
|:--:|---|:--:|---|
| 1 | CarA200 | 16 | **MsnDecoder** |
| 2 | BoxP100 | 17 | BoxC230 |
| 3 | CarA300 | 18 | BoxP210 |
| 4 | BoxP200 | 19 | BoxC250 |
| 5 | CarA301 | 20 | HUD |
| **6** | **BoxP300** ← Prado | 21 | BoxP220 |
| 7 | BoxP400 | 22 | BoxP230 |
| 8 | BoxP500 | 23 | CarA301 |
| 9 | BoxP700 | 24 | BoxC270 |
| 10 | CarA301 | 25 | BoxP701 |
| 11 | BoxP800 | 26 | BoxC280 |
| 12 | NV17 | 27 | BoxC290 |
| 13 | Bagoo | 28 | D107 |
| 14 | IM60BC | 29 | ZhongHang |
| 15 | BoxP900 | 30 | RuiYuanSWC |

---

## Prado vs Holden — same library, different adapter

The `libMcuCenter.so` binary is **byte-identical** between the Prado dump and the
Holden-derived reconstruction (MD5 `065d2cc4a3fb9ef1740e755e041db4a0`) — it ships
all 30 adapters. The MCU behaviour difference is **entirely config-driven** via
`McuType` in `MsnProductInfo.ini`:

| Setting | Prado dump | Holden base |
|---|---|---|
| `ProductId` | `Limcet-P306` | `Ksmart_DSP` |
| `ResourceName` | `Box-P301` | `Box-C211` |
| **`McuType`** | **6 → `BoxP300`** | **16 → `MsnDecoder`** |
| `ScreenType` | 1 | 3 |
| `SoundType` | 0 (none) | 4 (DSP) |
| `CanType` | 0 | 0 |

So Prado and Holden drive the MCU with **two different adapter classes speaking
two different serial protocols**. The reconstruction correctly keeps `McuType=6`
(`BoxP300`), matching the Prado's stock MCU. Running raw Holden (`McuType=16`,
`MsnDecoder`) points the wrong adapter at a `BoxP300`-speaking MCU.

### Protocol command-code diff (why some functions carry over and some don't)

**Verified by disassembling each adapter's `onRecvMcuProtocol` dispatch** (not by
counting `cmp` immediates — an earlier draft did that and mis-reported the codes).
The two adapters parse **different frame layouts** and switch on **different bytes**:

| | `BoxP300` (Prado) | `MsnDecoder` (Holden) |
|---|---|---|
| Min frame size | 4 (`getPackageMinSize`), header sig `0x2E` (`findPackageStartSig`) | 6 (checks `size ≤ 6` → reject) |
| **Command byte position** | **offset 1** (`ldrb r3,[data,#1]`) | **offset 3** (`ldrb r6,[data,#3]`), with a group byte at **offset 2** (`0x13`/`0x16`) |
| **Command codes (traced switch)** | `0x00`–`0x06`, `0x0A`, `0x12`, `0x20`, `0x21`/`0x22`, `0x30`, `0x7F`, `0xE2`, `0xE4` | `0x01`–`0x04`, `0x10`, `0x11`, `0x13`, `0x14`, `0x22`, `0x32`, `0x33` |
| Version query | cmd `0xE4` → `echo … > /tmp/mcu_version` | (different path) |

So the frame **structure itself differs** — BoxP300 puts the command at byte 1;
MsnDecoder uses a group byte at byte 2 plus a sub-command at byte 3, and requires
a longer minimum frame. Even where individual code values coincide (e.g. `0x22`),
they sit at different offsets and mean different things. This is why a mismatched
adapter only partially works: overlapping low-level framing lets simple messages
through, but the command interpretation diverges. **For the Prado, `McuType` must
be `6`.**

---

## BoxP300 (McuType=6) — full command dispatch (disassembly-verified)

Every incoming command handler in `MCUAdapter_BoxP300::onRecvMcuProtocol`
(`0x0370d4`) traced individually. Frame: `byte[0]` = header sig `0x2E`, `byte[1]`
= command, min length 4. `MsnEvent` + `QCoreApplication::sendEvent` = the handler
posts an event into the UI app; `makeProtocolPackage` = it builds a reply back to
the MCU.

| Cmd | Handler | Function (from traced calls/strings) | Confidence |
|:--:|:--:|---|---|
| `0x00` | `0x37348` | default / ignored | high |
| `0x01` | `0x38584` | input event, reads `byte[3]` → posts `MsnEvent` type **`0x1013`** to app `0x191` (type in the `0x10xx` range = likely key/button) | med* |
| `0x02` | `0x37350` | handshake / reply builder (`makeProtocolPackage`) | med |
| `0x03` | `0x388b4` | status bitfield (`QBitArray`) | med |
| `0x04` | `0x37790` | **reverse radar / parking-sensor level** → `transRadarLevel` → `MsnEvent` | high |
| `0x05` | `0x38190` | status → `MsnEvent` type **`0x5018`** to app `0x191` | med* |
| `0x06` | `0x38360` | status **bitfield** (`byte[3]` via `QBitArray`) → `MsnEvent` type **`0x501A`** to app `0x190` | med* |
| `0x0A` | `0x37538` | **steering angle → dynamic reverse trajectory** (`"recv track:"`): `byte[3] bit0` = direction, `byte[4..5]` = 16-bit magnitude, scaled (float `vdiv`/`vmul`) to a signed angle that drives the bending guideline (`Guideline`, `CarTrackImgMaxAngle`, `GetAngle`) | high |
| `0x12` | `0x38144` | status, reads `byte[4]` → `MsnEvent` type **`0x5026`** to app `0x191` | med* |
| `0x20` | `0x3746c` | reply builder (`makeProtocolPackage`) | med |
| `0x21`/`0x22` | `0x37514` | `MsnEvent` | med* |
| `0x30` | `0x379f8` | **arkdata / display-config file I/O** (`/msnprofile/`, `arkdata/`, `QDir`, `QFileInfo`) | high |
| `0x7F` | `0x38e00` | **MCU version report** (`echo … > /tmp/mcu_version`, `system`) + arkdata name change (`GetArkdataChangeName`) | high |
| `0xE2` | `0x37c54` | firmware-update flow (`"End Update Mcu!"`, `GetTickCountMs`) | high |
| `0xE4` | `0x37160` | **firmware-update data packet** (`"recv update packageid:"`, `UpdateDialog`, `QFile`) | high |

\* **Input-event decode — what is and isn't resolved.** The library-side
interpretation is `libMcuCenter.so` on the **SoC** (not the STM32): each handler
reads its frame bytes and constructs an `MsnEvent(param, type, MsnEventType)` (ctor
`_ZN8MsnEventC1Ejj12MsnEventType`), then posts it into the Qt app. Traced facts:
each event command posts a **distinct type code** — `0x01→0x1013`, `0x05→0x5018`,
`0x06→0x501A`, `0x12→0x5026` — so they are **not** interchangeable, and they read
different frame bytes (above). `0x04` (radar) and `0x0A` (steering angle) are named
by unique calls/strings. **Not resolved:** the human meaning of the `0x50xx` type
codes. They aren't compared as immediates anywhere (`MsnCoreApp` routes them by
target-app without a switch I could map), and the `MsnEventType` enum names aren't
in the binaries — so labelling `0x05/0x06/0x12` as ACC/IGN vs reverse-flag vs
illumination would be a guess and is deliberately left open. (An earlier draft's
`0x1D` "main command" was a `cmp`-frequency artifact and is wrong.)

**Outbound** (`makeMCUProtocol` at `0x31e60`, called by `syncSettingDataToMcu` /
`translateApp` / etc.): the command byte is not passed as a nearby inline
constant, so the outbound command set is **not yet enumerated** — noted here as an
open item rather than guessed.

---

## Capturing the codes live on the device (to resolve the open items)

The two open items above — naming the `0x50xx` `MsnEventType` codes and the
outbound command set — can't be pinned from the binary alone but are easy to map
on a running unit by watching `/dev/ttyHS0` while physically toggling inputs
(Reverse, headlights/ILL, ACC, SWC buttons) and noting which command byte + event
fires for each.

**Device facts.** ARK1680 Linux/BusyBox, root over SSH (this project enables
`sshd`). Present: BusyBox `cat`, `dd`, `hexdump`, `microcom`. **Absent:** `strace`,
`stty`, `od`, `xxd`, `socat`. MCU link = `/dev/ttyHS0`; baud is the adapter default
(not in `MsnProductInfo.ini`) — try **115200**, then **38400**. `MsnCoreApp` holds
the port open, so a second reader races for bytes — plan around that.

### Method A — built-in MCU debug log (preferred; reuses the app's own parser)
The frame logging in `libMcuCenter.so` (`recvProtocolData`, `recv track:`,
`send msn mcu code!`, `Recv change arkdata name:` …) is gated by a flag file:

```sh
touch /data/mcudebug_flag        # gates MCU-protocol frame logging
touch /data/mcudebug_flag_msn    # gates the MSN-side logging
```
The app likely reads the flag at startup, so restart it (or reboot):
```sh
killall MsnCoreApp               # init/rcS respawns it
```
Then watch its debug output. Qt `qDebug` goes to stderr — on this unit that is the
**serial debug console** (`/dev/ttyS0`, 115200 8N1 — the console in README §2),
*unless* the launch script (`/etc/rc.d/rcS` / the MsnCoreApp start line) redirects
it to `/dev/null`. If it does, either edit that line to
`>> /data/msn.log 2>&1`, or run the app by hand from the SSH shell to see output:
```sh
killall MsnCoreApp ; cd <app dir> ; ./MsnCoreApp 2>&1 | tee /data/msn.log
```
Now toggle each input and record the `recv …` lines + frame bytes.

### Method B — raw UART byte sniff (exact bytes, no app parsing)
Stop the app first (otherwise it steals the bytes), then read the port. With no
`stty`, set the baud via `microcom`:
```sh
killall MsnCoreApp
busybox microcom -s 115200 /dev/ttyHS0            # interactive view; Ctrl-X to exit
# or log to hex + file:
busybox cat /dev/ttyHS0 | busybox hexdump -C | tee /data/ttyHS0.log
```
The MCU keeps sending periodic status frames with the app stopped, so you can still
toggle Reverse/ACC/lights and see them — you just lose the on-screen correlation.

### Method C — off-device hardware tap (ground truth, fully passive)
Probe the STM32↔SoC UART TX/RX with a logic analyzer / USB-serial sniffer at the
MCU baud. No software interference; requires opening the box.

### What to record and how to map it
Each frame is `[0x2E sig][cmd][payload…][checksum]`. For every physical action log
the **cmd byte (offset 1)** and payload, then cross-reference the BoxP300 table
above:
- Reverse gear → expect `0x0A` (steering angle/trajectory) + a status command
- Headlights/ILL, ACC on/off → one of the `0x50xx`-type status commands
  (`0x05`/`0x06`/`0x12`); seeing which one fires **names that `MsnEventType`**.
- SWC button → the `0x01` (`0x1013`) input-event command; the payload byte is the
  key code.

Toggling exactly one input at a time is the whole trick — it disambiguates the
`0x50xx` codes the static disassembly could not.

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
**CAN:** YES — bidirectional CAN frame encode/decode — **disassembly-verified**:
`writeCanBusData` (`0x6c260`) → `makeCanBusProtocol` (`0x6bf88`, builds
`[hdr][type][sub][payload][checksum]`) → `ProtocolUtils::writeDatas` (sends the
CAN frame to the MCU over the serial port). All six CAN/SWC methods present.  
**Update:** None  
**Resources:** `%1resources/Box-P230.rcc` (contains `xbs_honda/` Honda-specific UI)  
**McuType:** 22.  
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
**Notes:** The P300-series adapter. **Used by Limcet-P306 — `McuType=6`, confirmed
from the factory jump table (see "Prado vs Holden" above).** No steering wheel ADC
or CAN support. MCU sends pre-decoded key events via the serial protocol. Frame
header sig `0x2E`, command byte at frame offset 1 (codes `0x00`–`0x06`, `0x0A`,
`0x12`, `0x20`–`0x22`, `0x30`, `0x7F`, `0xE2`, `0xE4`; `0xE4` = version query).

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
**McuType:** **16 — the Holden base config (`Ksmart_DSP` / `Box-C211`) selects this.**
**Protocol:** requires min 6-byte frame; group byte at offset 2 (`0x13`/`0x16`),
command byte at offset 3 (codes `0x01`–`0x04`, `0x10`, `0x11`, `0x13`, `0x14`,
`0x22`, `0x32`, `0x33`) — a **different frame layout and command set from
`BoxP300`** (command at offset 1), so the two are not protocol-compatible (see
"Prado vs Holden" at the top).  
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

**CAN bus confirmed** — an NXP TJA1042 high-speed CAN transceiver is populated
adjacent to the STM32F105 on the board. The MCU reads Toyota Prado steering wheel
button presses directly from the vehicle CAN bus, decodes the Toyota-specific CAN
message IDs, and forwards key events to the ARK1668 via `/dev/ttyHS0`.

The ADC SWC path (resistive voltage divider) is **not used** for steering wheel
controls on this device. The `EnableSWCSwitchHardware` / FK key-learning feature
in the ARK1668 software does not apply.

Because touch events already work (proving MCU↔ARK comms is active), the SWC failure
is a **key mapping / learning issue**, not a hardware or protocol problem.

### Confirmed CAN bus architecture

The NXP TJA1042 CAN transceiver is physically present on the board, wired between
the STM32F105 bxCAN controller and the vehicle harness CANH/CANL pins. This is a
complete hardware CAN bus circuit.

**The SWC signal path is:**
```
Toyota Prado steering wheel button press
  → CAN bus (body CAN, typically 500 kbit/s)
  → Harness CANH/CANL wires → head unit connector
  → NXP TJA1042 transceiver
  → STM32F105 bxCAN controller
  → Limcet-V1.0-1302 MCU firmware (CAN ID decode)
  → UART (/dev/ttyHS0)
  → ARK1668 running libMcuCenter.so (McuType=6)
  → application key event
```

### Why SWC doesn't work — root causes

**1. Physical wiring (most likely)**  
The CANH/CANL wires from the Toyota Prado harness connector must be plugged into
the head unit's CAN input. On many installations these are left unconnected. Check
the 2-wire CAN pair on the vehicle harness — typically twisted pair, often white/orange
or green/yellow depending on region.

**2. MCU firmware CAN ID table**  
The Limcet-V1.0-1302 firmware must contain the correct Toyota Prado CAN message ID
and byte mapping for the steering wheel buttons. Toyota Prado 150 series SWC messages
are typically on CAN ID `0x25` or `0x026` at 500 kbit/s on the body CAN.  
If the MCU firmware was built for a different vehicle or has the wrong IDs, keys will
not decode even with correct wiring.

**3. CAN bus speed mismatch**  
Toyota body CAN runs at 500 kbit/s. The STM32 bxCAN must be initialised at the same
speed. If the MCU firmware uses a different baud rate, no frames will be received.

### Diagnosis via MCU Monitor

The advanced factory menu MCU Monitor shows raw data arriving from the MCU on
`/dev/ttyHS0`. With the steering wheel connected and buttons pressed:
- If data changes in the monitor → MCU is decoding CAN and sending key codes;
  the issue is key mapping in the ARK1668 software
- If nothing changes → CAN frames are not being decoded by the MCU;
  check wiring first, then consider MCU firmware

### MCU firmware update

The STM32F105 can be re-flashed via:
- **USB DFU** (USB OTG port on the chip, if exposed on the board)
- **UART bootloader** (STM32 built-in bootloader on USART1, if accessible)
- **SWD/JTAG** debug interface (requires STLink or J-Link probe)

The `mcuupdate4/` path referenced in `libMcuCenter.so` suggests the ARK1668 can push
MCU firmware updates from the SD card or USB drive — if the MCU supports this OTA
path over the existing `/dev/ttyHS0` link.

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
