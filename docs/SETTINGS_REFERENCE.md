# Settings Reference

**Status:** Reference
**Last Updated:** 2026-08-03

## Overview

Complete key-by-key reference for the two INI files that configure an
Arkmicro "MSN" head‑unit / CarPlay box (Limcet‑P306, Box‑C235, Ksmart, etc.).
These are the files the factory tool and the running application read to decide
*what hardware the box has* and *how it behaves*.

## Where the files live

Each file exists in **two physical copies** that serve different roles:

| Layer | Path | Role |
|-------|------|------|
| **Provisioning copy** | `msn_factory_configs/*.ini` inside the update ZIP/SD payload | Applied by the factory/update tool **at flash time**; copied into `/msnprofile` to override the baked‑in defaults for a specific product SKU. |
| **Rootfs baked‑in copy** | `/msnprofile/MsnProductInfo.ini`, `/msnprofile/FactoryConfig.ini` | The defaults compiled into the shipped `rootfs.img`. Read **at every boot** by `MsnCoreApp` / `Launcher` — but see [Userdata caching](#userdata-caching-settingconfig) below, this is not the whole story. |

Both files are flat `key=value` text. `MsnProductInfo.ini` uses a single
`[General]` section. `FactoryConfig.ini` uses four sections — `[General]`,
`[BlueTooth]`, `[Sound]`, and `[Radio]` — though some SKUs (e.g. the Box‑C235
rootfs copy) place the general keys at the top of the file *before* any header,
which the reader treats as the default `[General]` section. A leading `#`
comments a line out and falls back to the firmware default.

## Load sequence

The values reach the running system in a fixed order — first once at the factory,
then again on every power‑up:

**Provisioning (once, at flash/update time)**

1. The update tool flashes `rootfs.img`, then reads the SKU's
   `msn_factory_configs/*.ini` from the payload.
2. It copies those into `/msnprofile/`, overwriting the rootfs baked‑in copies —
   this is what turns a generic image into a "Toyota Box‑C235" or "Limcet‑P306".

**Boot (every power‑up, by `MsnCoreApp`)**

3. **Read `MsnProductInfo.ini` first** — establishes the *hardware profile*
   (`ScreenType`, `ResolutionType`, `McuType`, `BlueToothType`, port names…).
4. Use `ScreenType` + `ResolutionType` to select and apply the matching
   **`arkdata` display preset** → panel comes up (see
   [`ARKDATA_VARIANTS.md`](1.7_DISPLAY_SUBSYSTEM.md)).
5. Use `McuType` to load the matching **MCU adapter** and its `KeyMaps-NN` /
   `Knob-NN` blocks (see [`1.3_MCU_ADAPTERS.md`](1.3_MCU_ADAPTERS.md)); use
   `BlueToothType` to bring up the BT stack per `/usr/config.ini`.
6. **Read `FactoryConfig.ini` next** — applies *behaviour and branding* on top of
   the now‑known hardware (projection, media/audio, UI, factory menu, Bluetooth).
7. Launch the UI (`Launcher` / projection) using the resolved settings.

> Because step 3 gates steps 4–6, a wrong `MsnProductInfo.ini` (e.g. bad
> `ScreenType`) produces a blank/garbled display before `FactoryConfig.ini` is
> ever reached.

## File interaction map (2026-08-03)

The two‑tier provisioning story above is accurate but incomplete — there are
more files in play than just the two INIs, and some of them interact in ways
that aren't obvious from reading either file alone. Full picture, gathered
across this project's disassembly work on `SoundType`/`Language`/`ScreenType`/
`CanType`/etc.:

| File | Location | Written by | Read by |
|---|---|---|---|
| `MsnProductInfo.ini` | `/msnprofile/` (rootfs) | Factory/update tool, at flash time only | `MsnCoreApp`, every boot |
| `FactoryConfig.ini` | `/msnprofile/` (rootfs) | Factory/update tool, at flash time only | `MsnCoreApp`/`MsnFirstInit`, every boot — for most keys; see the caching caveat below |
| `/data/msncfg/Setting.config` | userdata partition | `MsnCoreApp`, once, at first provisioning | `MsnCoreApp`, **every subsequent boot, in preference to the ini files** |
| `/msnprofile/arkdata.ini` | `/msnprofile/` (rootfs) | Factory tool at flash time, **and** rewritten live by `libMcuCenter.so`'s MCU-protocol handlers | `MsnCoreApp` (LCD panel init); separately, **U-Boot** reads its own unrelated copy — see below |
| `sd_bootable/arkdata.ini` | SD card FAT partition (this project's own) | This project, manually | U-Boot only, pre-Linux |

**Userdata caching (`Setting.config`).** Editing an ini value is not
guaranteed to change live behavior. `Setting.config` in userdata is a cache
that, once populated, **wins over the ini files on every subsequent boot** —
the ini only seeds it the very first time a device provisions. Confirmed for
`Language` (documented in §2.4 below) and functionally for `SoundType`
(`project_mic_capture_investigation` — editing the ini alone didn't stick
across a real device that had already provisioned). To force a value to take
effect on an already-provisioned device, either edit `Setting.config`
directly or trigger a factory reset (`fw_setenv factory_reset 1`), which
wipes all of `/data` and forces a fresh reseed from the ini — not something
to do casually, it also wipes every other saved userdata setting.

**`arkdata.ini` is genuinely bidirectional.** It isn't a static factory
timing table read once — `libMcuCenter.so`'s MCU-adapter protocol parsers
(`MCUAdapter_Bagoo`/`BoxC270`/`ZhongHang`, one per physical MCU board
variant) **write** `ScreenType` and a full LCD timing block into its
`[DISPLAY_INTERFACE]` group at runtime, based on which MCU board actually
handshakes with the unit at boot. The value you observe in this file at any
given moment may reflect what the connected MCU reported, not just what was
flashed — see §1.2 (`ScreenType`) below.

**Two files named `arkdata.ini` with zero relationship to each other.** The
Linux-side `/msnprofile/arkdata.ini` above is entirely separate from this
project's own `sd_bootable/arkdata.ini` (same format, same key names, even
overlapping display-timing keys) — U-Boot reads its copy before Linux ever
boots, to build the splash screen and patch the kernel DTB (see the
`BootInterrupt` toggle work in this project's U-Boot history); Linux never
touches U-Boot's copy and vice versa. Easy to confuse when debugging a
display issue — always check which layer (pre-boot U-Boot vs. running Linux)
is actually relevant before editing either one.

**Build-time layering adds a third copy of the two main INIs.**
`firmware_overlay/msnprofile/*.ini` wins unconditionally over
`firmware_source/mtd6_rootfs/msnprofile/*.ini` on every `build_bootable_sdcard.sh`
run (rsynced on top, last write wins). This caused a real regression once
already: a `SoundType` fix landed in the `mtd6_rootfs` copy but not the
overlay copy, so every rebuild silently reverted the live device back to the
old value. **When changing a setting in this repo, both copies need updating**
(or just the overlay copy, if the intent is a permanent project-wide
deviation from the base rootfs dump — check both before assuming a change
shipped).

## Sources for the value tables

Every value below was observed in a real config in this repo. Product columns:

| Column | Firmware source |
|--------|-----------------|
| **Holden** | `Holden firmware update/` (Ksmart_DSP / Box‑C211, MCU 16) |
| **Limcet P306 (orig)** | `Prado firmware dump/` — the physical device dump (Limcet‑P306) |
| **P306‑2025** | `P306 2025 Firmware Update/` (Limcet‑P306, 2025‑11‑22 build) |
| **C235‑2025** | `CarSyncTech Toyota/CSTech‑202511‑IP17.zip` (Box‑C235 Toyota, 2025‑11‑22 build) |

> **Confidence:** key *names* and *observed values* are verbatim from the files.
> Descriptions of self‑evident keys are firm. Numeric/hex enum meanings marked
> *(inferred)* are best‑effort from cross‑product behaviour and naming — the
> firmware does not ship a documented enum table for them.

---

# 1. `MsnProductInfo.ini` — hardware / product profile

All keys are under `[General]`. This file tells the software what silicon and
peripherals the board has; it is read first at boot (load‑sequence step 3) and
getting it wrong (e.g. `ScreenType`) produces a blank or garbled display before
`FactoryConfig.ini` is ever reached. Grouped by function below.

## 1.1 Product identity & resources

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `ProductId` | `Limcet-P306`, `Box-C235`, `Ksmart_DSP` | Product/model identifier string. Cosmetic + used for update matching and log tagging. |
| `ResourceName` | `Box-P301`, `Box-C235`, `Box-C211` | UI resource‑pack / board family. Selects which asset set and `arkdata` group letter is used (Box‑P → group P, etc.). Replaced by `LauncherName` on Box‑C235. **Full literal string survey (2026-08-03)** across every binary found: `Box-C211`, `Box-C212`, `Box-Icon`, `Box-P201`, `Box-P260`, `Box-P301`, `Box-P307`, `Box-P700`, `Box-P702`, `Box-P800`, `Box-P900`, `Box-Test` (spread across `libLauncher-Box.so`, `libMcuCenter.so`, `MsnCoreApp`) — these are the full set of recognized values. **Anomaly**: `Box-C235` (one of this doc's own observed real-world values) does **not** appear as a literal anywhere in this scan — either matched via a prefix check (e.g. `startsWith("Box-C2")`) rather than an exact string, or sourced from somewhere this static scan didn't cover. Not resolved. |
| `LauncherName` | `Launcher-Box` | Which launcher UI binary to start (Box‑C235 only). Newer alternative to `ResourceName`‑driven selection. |
| `ProductType` | `2` (P306), `3` (C235/Holden) | Product class code. `2` = P306‑class, `3` = C235/C211‑class. *(inferred)* |

## 1.2 Display

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `ScreenType` | `1`, `3` | Panel interface type. Per `arkdata.ini` legend: `RGB565=1, RGB888=2, LVDS=4, VGA=8, CVBS=16, YPBPR=32, ITU656=64, ITU601=128`. `1`=RGB565 panel. **Disassembly-confirmed 2026-08-03, but as a WRITE not a read**: `MsnCoreApp` only registers the string name in a generic tag-relay table — the real logic is in `libMcuCenter.so`'s `MCUAdapter_Bagoo`/`MCUAdapter_BoxC270`/`MCUAdapter_ZhongHang::onRecvMcuProtocol()` (the MCU-protocol parsers for three different MCU board variants), which on a particular MCU message writes `ScreenType=2` (RGB888, hardcoded in the reached branch) into `/msnprofile/arkdata.ini`'s `[DISPLAY_INTERFACE]` group, alongside a full sibling timing block (`Format`, `LCD_CLOCK`, `CLKSource/Freq/Polarity/DIV1/DIV2`, `Width`/`Height`/`HFP`/`HSW`/`HBP`/`IHS`/`VFP`/`VSW`/`VBP`/`IVS`/`IOE`) — confirms the legend really is treated as this bitmask by real code. So `MsnProductInfo.ini`'s own `ScreenType` value may be overwritten at runtime by whichever physical MCU board is actually connected, not purely a static factory default. These are large (~KB-scale) functions with many message-subtype branches — only the one reached branch (writing `2`) was confirmed; other branches may write other legend values for other MCU message types, not fully enumerated. |
| `ResolutionType` | `1` | Resolution preset index; selects the timing block within the chosen `arkdata` preset. **Confirmed dead end (2026-08-03, triple-checked across 3 separate passes)**: the literal string `ResolutionType` does not exist anywhere in any binary in `usr/bin/*`/`usr/lib/*.so`. `MsnProductInfo.ini`'s own filename string in `MsnCoreApp` has zero code references either. The only `*Resolution*` symbols found are unrelated (libjpeg/mtdev, AA protobuf `VideoCodecResolutionType`, and `MsnApplication::getSResolutionSize/Name(SResolutionCode)` — a completely different "app icon layout resolution" concept for the Launcher UI, not this ini key). Whatever reads this key does so through an indirection not resolvable via static analysis alone (or it's genuinely vestigial). |

## 1.3 Peripheral hardware

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `McuType` | `6` (P306), `16` (C235/Holden) | Which MCU/steering‑wheel adapter protocol the box speaks over `MCUPortName`. Also selects the matching `KeyMaps-NN`/`Knob-NN` block. **Full 1–30 value table disassembly-confirmed** (`MCUAdapter::getAdapterInstance(McuType)`, `libMcuCenter.so` `0x25e40`, same jump-table idiom as `CanType` below) — see [`1.3_MCU_ADAPTERS.md`](1.3_MCU_ADAPTERS.md) for the complete list. `6`=`BoxP300` (this device's real adapter). |
| `BlueToothType` | `5`, `6` | Bluetooth module/stack. `5` = older module; `6` = newer RTL BLE‑capable module (matches the 2025 BC6/BLE additions in `usr/config.ini`). *(inferred — see note below, the disassembly trail doesn't close cleanly)* |
| `RadioType` | `0` | FM/AM tuner type. `0` = none. |
| `CanType` | `0` | Built‑in CAN decoder type selecting a `libCanBus.so` adapter class. **Full 0–16 value table disassembly-confirmed** 2026-08-03 — see [`1.2_CANBUS.md`](1.2_CANBUS.md) for the complete class table and the live-tested finding that `CanType=1` breaks touch/knob input (constructs a wrong-vendor HVAC-capable adapter that fights the MCU for its own UART port). `0`=none/MCU handles it (current, correct value here); `9`=`CanBus_Raise_Toyota` (unused on this device). **Do not set to anything other than `0`.** |
| `SoundType` | `0`, `2`, `3`, `4`, `5`, `128` | Audio routing/codec profile selector. **Full value table disassembly-confirmed** (`SoundAdapter::getInstance(SoundICType)`, `libMsnSound.so`): `2`/`4`=`Sound_PT2312` (Princeton PT2312), `3`=`Sound_BD37033` (Rohm BD37033), `5`=`Sound_MCU`, `128`(`0x80`)=`Sound_MCU_OnlyEQ`, anything else incl. `0`=no adapter constructed. `SoundType=0` is what this device currently ships with — no adapter, and as a side effect the only value confirmed to also let the mic work; `3`/`5` both construct a real adapter and both break the mic (mechanism not fully understood — see `1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md`/project memory). |
| `TouchScreen` | `0` | Touch controller class. `0` = none / handled by panel driver. |
| `WLANType` | `3` | Wi‑Fi module type. *(inferred)* |

**Note on `BlueToothType` (2026-08-03, two disassembly passes):** traced `BTSenderAdapter::getInstance(BTSenderType)` (`libBTSender.so`, `0x7144`) — it constructs exactly **one** class, `BTSenderAdapter_Blueware`, only when its argument is `1`; anything else falls through to an error/log path with no adapter. Its one call site (`libSetting.so`, `AudioSenderPanel::init()` `0x8aaa0`) reads a runtime app-tag, `MsnApplication::getTag(AppTagId=0x1000f, default=0)`, and passes that value **directly** as `BTSenderType` — it never reads the raw `BlueToothType` ini value at all. A follow-up pass exhaustively scanned every `setTag(AppTagId, QVariant)` call site across the entire firmware (`MsnCoreApp` + every `usr/lib/*.so`, 16 sites total, all in `libSetting.so`, decoding each site's `movw`/`movt` immediate) — **none of them ever construct `AppTagId=0x1000f`**. `getTag`/`setTag` themselves (confirmed at `libMsnCommons.so` `0x3e860`/`0x3fb98`) are a plain in-process map with no ini-fallback logic — an untagged read just returns the caller's coded default. **Conclusion: within this firmware's own binaries, `BTSenderAdapter::getInstance()` is only ever called with the implicit default `0`, meaning the Blueware BT-audio adapter is never actually constructed via this path at all** — `BlueToothType`'s `5`/`6` values control something else entirely (most likely `usr/config.ini`'s own BT module/AT-command selection, not this app-layer C++ class). Don't assume `BlueToothType` maps onto `BTSenderType` in any way.

## 1.4 Serial ports

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `MSNEryPortName` | `"/dev/ttyS2"` | Serial port for the secondary ("MSN Ery") peripheral link. |
| `MCUPortName` | `"/dev/ttyHS0"` | Serial port to the companion MCU (steering‑wheel/CAN/power). |

## 1.5 Feature flags

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `KeyRedefine` | `1` | Box‑C235 only. Enables steering‑wheel key remapping via the `KeyMaps-NN` blocks in `FactoryConfig.ini`. |

## 1.6 Product profile at a glance

| Key | Holden | Limcet P306 (orig) | P306‑2025 | C235‑2025 |
|-----|:------:|:------------:|:---------:|:---------:|
| `ProductId` | Ksmart_DSP | Limcet‑P306 | Limcet‑P306 | Box‑C235 |
| `ResourceName` | Box‑C211 | Box‑P301 | Box‑P301 | *(→ Launcher‑Box)* |
| `ProductType` | 3 | 2 | 2 | 3 |
| `ScreenType` | 3 | 1 | 1 | 1 |
| `McuType` | 16 | 6 | 6 | 16 |
| `BlueToothType` | 6 | 6 | 5 | 6 |
| `SoundType` | 4 | 0 | 128 *(pkg)* | 3 *(pkg)* / 0 *(rootfs)* |
| `KeyRedefine` | — | — | — | 1 |

---

# 2. `FactoryConfig.ini` — behaviour, branding & engineering

The large file. Grouped below by function; the **Section** column is the INI
section the key lives under. Some SKUs comment keys out with `#` (shown as
*[commented]*) to fall back to the firmware default.

## 2.1 Identity & branding — `[General]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `VehicleName` | `"HOLDEN"`, `"Limcet Box"`, `"TOYOTA"` | Brand/vehicle name shown in the UI and system labels. **⚠ Command‑injection sink** — see [§4](#4-security-notes). |
| `HomeIconLabel` | `"HOLDEN"`, `"HOME"`, `"TOYOTA"`, `"Limcet Box"` | Text label under the projection/home icon. |
| `SysVersionLabel` | `"Limcet_"` | Prefix prepended to the firmware version string in the About screen. |
| `CarPlayIconPath` | `"/msnprofile/icon_120x120.png"` | Path to the 120×120 OEM icon shown inside CarPlay. |

## 2.2 Phone projection & connectivity — `[General]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `AutoStartCarLink` | `1` | Auto‑launch phone projection (CarPlay/Android Auto) when a phone connects. |
| `IphoneLinkType` | `2`, `3` | iPhone connection mode. **Disassembly-confirmed** (`MsnCoreApp::onUSBPhoneStatusChange`, `0x2a4c4`: `bic r3,r3,#2; cmp r3,#1; bne <exit>`) — only **bit 0** of the value matters, bit 1 is explicitly masked off. Any odd value (`1` or `3`) triggers the phone-link UI event; any even value (`0`, `2`) is a no-op for this path. So `2`=inert, `3`=active — matches this file's own observed values exactly. |
| `AndroidLinkType` | `2`, `3`, `6` | Android connection mode. **Disassembly-confirmed**, two independent consumption sites in `MsnCoreApp`, both reading the same cached value: `onUSBPhoneStatusChange` (`0x2a3d0`) dispatches `1`/`4`→one UI event, `2`/`6`→a second event (`bic r2,r3,#4; cmp r2,#2` catches both), `7`→a third event, anything else (incl. `0`, `3`, `5`) falls through to a default/fallback path; `onFirstInit` (`0x2f6fc`) dispatches the *same* raw value slightly differently: `1`/`3`/`4`→one app-state, `2`/`5`/`6`→a separate large handler, `7`→another state — note the two sites do **not** treat `3` identically (fallback in one, active in the other), a real inconsistency worth being aware of if testing this value. Exact semantic meaning of the internal event/state numbers (5/13/19/29) wasn't recoverable — no `MsnEventType` enum ships as symbols anywhere in this firmware. |
| `MirroringLinkType` | `1`, `2` | Screen‑mirroring protocol variant. **Disassembly-confirmed 2026-08-03** (`MsnCoreApp::onFirstInit`, default `1`; `libMsnMirrLink.so`'s own copy of the string turned out to be pure UI-label material, confirmed via its `.dynsym` only exporting generic Qt widget classes). Real logic: `0` = no-op; any nonzero value only takes effect if `MsnApplication::getMsnProductValue("ProductId", "")` equals `"Car-A311"` or `"Car-A318"` — two specific factory SKUs, neither of which is this Limcet P306/Holden-derived unit's `ProductId`. Only on those two SKUs does it enable a MirrorLink home-screen app tile. **Practical implication: this setting has no effect at all on this device regardless of its value**, since our `ProductId` never matches the hardcoded gate. |
| `MirrorLinkType` | `2` | Box‑C235 spelling of the same setting (note: no second `i`). Treat as an alias. |
| `EnableMirroring` | `1` | Enable the phone‑mirroring feature (Holden). |
| `EnableBackgroundMode` | `1` | Keep the projection session alive in the background when another source is shown. |
| `LinkFullScreen` | `1` | Projection renders full‑screen (no UI chrome). |
| `ForceAPMode` | `1` | Force the Wi‑Fi into SoftAP mode for wireless projection. |
| `DisableAAutoBluetooth` | `1` *[commented]* | Stop the box auto‑managing Bluetooth for Android Auto. |
| `DisablePhoneLinkAudio` | `0`, `1` | `1` = do not route projection audio through the box (audio goes via the head unit/BT instead). |
| `EnableBackToCar` | `0` | Show/allow a "back to car UI" control from within projection. |
| `ModeAppMemType` | `0` | Memory‑allocation mode for the projection app. **Disassembly-confirmed** (`MsnCoreApp::onFirstInit`, default `3` if `MsnApplication::isBoxProduct()` else `0`) — a 2-bit mask, stored per-instance: bit 0 gates whether `onSaveStartupAppConfig()`/`onLoadStartupApp()` persist/restore the last-opened app across boots; bit 1 gates whether `onModeAppRestoryTimer()`'s idle-timeout "restore previous app screen" behavior runs. `0` (observed here) = both disabled. |
| `AndroidAutoUseTS` | `1` | Feed touchscreen input to Android Auto. |

## 2.3 Media & audio — `[General]` / `[Sound]`

| Key | Section | Observed values | Meaning |
|-----|---------|-----------------|---------|
| `AutoStartMedia` | General | `1` | Auto‑start the media/BT‑audio app on boot/connect. |
| `EnableMediaMode` | General | `1` | Enable the built‑in local media player. |
| `EnableMusic` | General | `0` *[commented]* | Enable the music app. |
| `EnableVideo` | General | `0` *[commented]* | Enable the video app. |
| `EnablePhoto` | General | `0` | Enable the photo viewer. |
| `IgnoreMediaKey` | General | `0`, `1` | `1` = ignore steering‑wheel media (next/prev/play) keys so the head unit handles them. |
| `Volume` | General | `20`, `32` | Default startup volume. |
| `ReversingVolumeCut` | General | `0`, `70` | Percentage volume attenuation while in reverse. `0` = no cut. |
| `AECDelay` | General | `50`, `150` | Acoustic‑echo‑canceller delay (ms) for hands‑free mic. |
| `AUXType` | General | `2` | AUX input hardware type. **Disassembly-confirmed** (`libAvin.so`, `AvinWindow`'s constructor, default `3`) — a 2-bit mask: `(AUXType&3)==3`→`setAppId(9)`, else bit0 set→`setAppId(17)`, else bit1 set→`setAppId(18)`, else (`0`)→no app registered. Observed value `2` (bit1 only) → `setAppId(18)`. |
| `AUXSource` | General | `2` | Which source index maps to AUX. **Disassembly-confirmed**, same constructor, immediately after `AUXType` resolves (default `1`) — passed directly, unmodified, to `AvinWindow::setDVRSource(int)`. Plain channel index, not an enum; observed value `2` selects DVR/AUX input channel 2. |
| `EnableAvin` | General | `0` | Enable the AV‑in (rear/camera video) source. |
| `EnableAircondition` | General | `1` | Enable the climate/air‑condition info screen. |
| `DoorPrompt` | General | `0` | Show door‑open prompts/animations. |
| `SysChannel` | Sound | `0` | System audio channel routing index. *(inferred)* |
| `BTChannel` | Sound | `2` | Bluetooth audio channel routing index. *(inferred)* |
| `InputGain2` | Sound | `6` | Mic/line input‑gain calibration, input #2 (Box‑C235). |
| `InputGain3` | Sound | `8` | Input‑gain calibration, input #3. |
| `InputGain4` | Sound | `8` | Input‑gain calibration, input #4. |
| `InputGain5` | Sound | `8` | Input‑gain calibration, input #5. |

## 2.4 UI, display & region — `[General]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `Language` | `4097`-`4111` | UI language code. **Confirmed 2026-07-26 via disassembly** of `libMsnCommons.so`'s `GetLanguageValueList()`/`GetLanguageNameList()` (the real ordered value↔name table `MsnCoreApp` builds at runtime) — corrects the earlier inferred guess that `4096` was English/default; `4096` actually decodes to an unrelated/anomalous string (possibly a reserved "system default" slot, not confirmed) and should not be used. Full confirmed table: `4097`=English, `4098`=简体中文 (Chinese Simplified), `4099`=繁體中文 (Chinese Traditional), `4100`=Português brasileiro, `4101`=한국어 (Korean), `4102`=Español, `4103`=Dansk, `4104`=Protuguês, `4105`=Italiano, `4106`=עברית (Hebrew), `4107`=Русский язык, `4108`=Français, `4109`=Türkçe, `4110`=Deutsch, `4111`=Dutch. `firmware_overlay/msnprofile/FactoryConfig.ini` sets `Language=4097` (English) as this reconstruction's default — the base `firmware_source` rootfs ships it commented out, and a live device capture showed it explicitly set to `4098` (Chinese Simplified). **Important, found 2026-07-27: this `FactoryConfig.ini` key is NOT read live.** It only seeds `/data/msncfg/Setting.config` once, at first provisioning (evidence: the userdata seed's `Setting.config` already carries `AutoStartCarLink` under a different value representation than `FactoryConfig.ini`'s copy, implying a one-time copy/translate step) — on an already-provisioned device, only editing `Setting.config` directly changes the live language; editing `FactoryConfig.ini` has no effect. See `firmware_source/mtd7_userdata/msncfg/Setting.config`. |
| `DisableWindowEffect` | `1` | Disable window/transition animations (Chinese comment: 禁用转场动画). |
| `ScreenRatio` | `0` | Aspect‑ratio handling mode. **Disassembly-confirmed** (`usr/bin/MsnFirstInit`, default `0`) — `ScreenRatio ∈ {1..5}` forces one of five preset aspect-ratio constants (a fixed table of `double`s) when computing display-scaling factors; any other value including the observed `0` skips the override entirely and uses the panel's actually-measured aspect ratio instead (`getScreenSize()`). So `0` = auto-detect, `1`-`5` = force a specific preset ratio. |
| `EnableBackLight` | `0` | Software backlight control toggle (Box‑C235). |
| `EnableDateTime` | `0` | Show the date/time widget (Box‑C235). |
| `ListItemFocusLoopMode` | `0` | Whether list focus wraps around at the ends. |
| `RightHandCarDriver` | `0`, `1` | `1` = right‑hand‑drive UI layout. |
| `CarTrackImgMaxAngle` | `20` | Max steering angle drawn on the reversing‑guideline overlay. |
| `TouchCalibrateAction` | `0` | Touch‑calibration trigger mode. **Disassembly-confirmed** (`MsnCoreApp`, `TouchKeyMonitor::startMonitor()`, default `1`) — plain boolean enable flag, not a multi-value enum: nonzero arms a 10-second touch-idle `QTimer` that posts a touch-calibration event and opens `/dev/input/event0`; `0` (observed here) skips that setup entirely, feature disabled. |

## 2.5 Steering‑wheel & rotary‑knob keys — `[General]`

These blocks are indexed by `McuType`: `KeyMaps-16` / `Knob-22` apply when
`McuType=16`, etc. A `"0"` / `"0=0,0=0"` value means "unmapped".

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `EnableSWCSwitchHardware` | `1` | Enable the **ADC voltage‑divider** hardware path for reading steering‑wheel keys (a dedicated SWC wire sampled by the SoC). On this board the STM32 MCU actually decodes SWC off the CAN bus and forwards key events over UART instead — so this flag's ADC path is unused here. See [`1.3_MCU_ADAPTERS.md`](1.3_MCU_ADAPTERS.md) and the README hardware notes. |
| `EnableFKLearn` | `1` | Enable steering‑wheel key "learn" mode (Box‑C235). |
| `KeyMaps-16` | `"0"` … `"0x02000013=0xFF0384", …` | ADC key‑code → function map for MCU type 16. Each pair maps a raw steering‑wheel key code to a firmware key action. |
| `KeyMaps-17` | `"0"` | Same, for MCU type 17. |
| `Knob-20` / `Knob-22` / `Knob-23` | `"0=0,0=0"`, `"0x01000014,0x01000012"` | Rotary‑encoder CW/CCW key‑code mappings for the given MCU type. |

## 2.6 Factory / engineering menu — `[General]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `FactoryPassword` | `0000`, `8818` | Password to enter the hidden factory/engineering menu. |
| `DisableFactorySetItems` | `"1,2,3,4,5,6"` | Hide the listed factory‑menu item indices from the user. |
| `DisableSetMCUType` | `1` | Hide the "MCU type" selector in the factory menu (lock the adapter). |
| `SettingItemTypes` | `"0x83,0x86"`, `"0x83,0x84,0x85,0x93,0x94,0x80,0x81,0x82"` | Which car‑setting item types (hex IDs) to expose in the settings UI. |
| `CarSetDefValue` | `"0x7=1","0x8=1"`, `"0x83=2"`, `"0x83=2","0x93=1","0x94=1"` | Default values assigned to those car‑setting item IDs (`0xID=value`). |
| `MCUUpdateName` | `"toyotaCarplayFW2.0.bin"` | Filename of the companion **STM32 MCU** firmware the box should flash (ties the head‑unit update to the dongle "ScreenFix" update). Box‑C235 only. |

## 2.7 Bluetooth — `[BlueTooth]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `DeviceName` | `"Limcet Box"`, `"Car Audio"`, `"Ksmart"` | Bluetooth advertised name. |
| `PairCode` | `8362`, `0000` | Bluetooth pairing PIN. |
| `AutoConnect` | `1` | Auto‑reconnect the last paired device on boot. |

## 2.8 Radio — `[Radio]`

| Key | Observed values | Meaning |
|-----|-----------------|---------|
| `RadioArea` | `7` | Radio region / band‑plan code (only meaningful when a tuner is fitted, i.e. `RadioType` ≠ 0). *(inferred)* |

---

# 3. Related settings files (documented elsewhere)

| File | Purpose | Reference |
|------|---------|-----------|
| `/msnprofile/arkdata.ini` + `arkdata/arkdataNN_X.ini` | LCD panel timing, clock dividers, touch‑key ranges. Selected via `ScreenType`/`ResolutionType`. The file carries its own inline enum legend. | [`ARKDATA_VARIANTS.md`](1.7_DISPLAY_SUBSYSTEM.md) |
| `/usr/config.ini` | Bluetooth "BC6" module AT‑command / indicator protocol map. **Identical across all 2025 products**; the 2025 build adds BLE (`SPP_BLE_ADV`, `IND_DEVICE_VENDOR_NAME`). Header marks it read‑only (此文件禁止更改). | — |
| `msn_factory_configs/MsnProductInfo.ini` etc. | Provisioning overrides applied at flash time (see top of this doc). | — |

---

# 4. Security notes

**`VehicleName` is a shell command‑injection sink.** In the `Prado firmware
dump` (the physical device), `VehicleName` is set to:

```
"Limcet Box$(d=/data/ssh;mkdir -p $d; openssl genrsa 2048 > $d/ssh_host_rsa_key; … ;
  printf 'PermitRootLogin yes\nPermitEmptyPasswords yes\n…' > $d/sshd_config;
  /usr/bin/sshd -f $d/sshd_config)"
```

The `$(...)` is command substitution: when `MsnCoreApp` passes `VehicleName`
through a shell, it generates SSH host keys and starts `sshd` with root login and
empty passwords enabled. This is the repo's own **rooting technique** (cf.
`msn_autocopy_payload/`), not a factory value — but it documents that
`VehicleName` (and any other string field echoed to a shell) is unsanitised and
**executes as root**. The stock update packages (P306‑2025, C235‑2025) ship a
plain `VehicleName`.

**`FactoryPassword`** is a weak, static PIN (`0000` on Toyota Box‑C235, `8818`
on Holden). Combined with the factory menu's `DisableSetMCUType=0` etc., anyone
with the PIN can change adapter/region settings. The Limcet‑P306 line omits
`FactoryPassword` and instead hides items via `DisableFactorySetItems`.