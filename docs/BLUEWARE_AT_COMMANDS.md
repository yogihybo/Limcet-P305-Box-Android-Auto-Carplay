# blueware AT Command Reference

Full reference for `/usr/bin/blueware`'s AT-command vocabulary (the Feasycom
BW121 module driver, controlled over `/dev/bw_serial` at 38400 baud — see
`custom_ui/src/hal/bluetooth.h`'s own top comment for the wire-level details).
Extracted via `strings firmware_source/mtd6_rootfs/usr/bin/blueware | grep
"^+" | sort -u` — **209 distinct command tokens**, the full set this binary's
AT-command dispatcher recognizes. This project has only ever needed and
tested a small fraction of them; this doc catalogues all of them for future
reference, clearly marking what's actually confirmed vs. what's inferred
from naming convention alone.

## Protocol basics (confirmed, not guessed)

- Wire format: `AT+<COMMAND>[=<args>]\r\n`, one command per line.
- Success reply: either a bare `OK`, or one or more `+<COMMAND>=<value>`
  lines (query responses), depending on the command.
- Failure reply: `ERR<NNN>` (e.g. `ERR002`, `ERR004`) — a numbered error
  code. **No error-code table exists anywhere in the binary** (no strings
  found near any `ERR` occurrence) — meanings below are inferred from
  observed context only, not documented anywhere.
- Unsolicited broadcasts: blueware pushes `+<COMMAND>=<value>` lines on its
  own for state changes (device connect/disconnect, incoming call, AA/CarPlay
  detection, etc.) — not just in response to a query. `hal::watch_bluetooth_broadcasts()`
  is this project's own generic listener for these.
- A parallel, unrelated framing (`[0xFA][arg1][arg2][arg3][len][payload...][chk][0xAF]`)
  exists on a **second, separate serial port** (`/dev/ttyS2`, 4800 baud) —
  not part of this AT-command vocabulary at all, see [`docs/1.3_MCU_ADAPTERS.md`](1.3_MCU_ADAPTERS.md).

## Status legend

| Marker | Meaning |
|---|---|
| **TESTED** | This project has actually sent this command on real hardware and observed a real response (may still be `ERR*`) — see the Notes column. |
| **BROADCAST-SEEN** | Observed arriving unsolicited on real hardware, never sent by this project. |
| **STRING-ONLY** | Confirmed present in the binary's command table via `strings`, but never exercised — form/semantics below are inferred purely from the command name and surrounding context, not verified. |

---

## Core / system

| Command | Form | Status | Notes |
|---|---|---|---|
| `BTEN=0`/`BTEN=1` | set | TESTED | Bluetooth radio enable/disable. Used by `hal::set_bluetooth_enabled()`. |
| `SCAN=0`/`SCAN=1` | set | TESTED | Discoverable/scan mode toggle. `hal::set_discoverable()`. |
| `PLIST` | query | TESTED | Lists paired devices, `+PLIST=<entry>` per device. `hal::list_paired_devices()`. |
| `ADDR` | query | TESTED | Local adapter Bluetooth address, `+ADDR=<mac>`. `hal::get_adapter_address()`. |
| `NAME=<name>` | set | TESTED | Sets the local device name shown to phones. `hal::set_device_name()`. |
| `PIN=<pin>` | set | TESTED | Sets the legacy pairing PIN. `hal::set_pairing_pin()`. |
| `DEVSTAT` / `DEVSTAT=` | query/broadcast | BROADCAST-SEEN | Device connection status; seen as an unsolicited broadcast on real hardware. |
| `PWRSTAT=` | broadcast | BROADCAST-SEEN | Power status broadcast, seen live (`+PWRSTAT=1`). |
| `RESTORE` | action | STRING-ONLY | Factory-reset the module's NVM, by name convention — **do not send speculatively**, no confirmed scope of what gets reset. |
| `REBOOT` | action | STRING-ONLY | Reboots the module itself (not the SoC). |
| `VER` / `VER=` | query/set | STRING-ONLY | Firmware version query; `=` form unclear (could be a version-gated feature flag, not necessarily "set version"). |
| `TEST` | action | STRING-ONLY | Likely a factory test-mode entry point — unconfirmed, don't send without more context. |
| `UARTCFG` | query/set | STRING-ONLY | UART port configuration (baud/parity/etc.) — this project already independently confirmed 38400/8N1 via `MCUAdapter_BoxP300::getPortSettings()` on the SoC side, not via this command. |
| `BAUD` | query/set | STRING-ONLY | Baud rate, likely overlaps with `UARTCFG`. |
| `LPM` / `LPMCFG` | query/set | STRING-ONLY | Low-power-mode toggle/config. |
| `DUT` / `DUT=` | action/broadcast | STRING-ONLY | "Device Under Test" mode — RF certification test mode, not for normal operation. |
| `SSP` | query/set | STRING-ONLY | Secure Simple Pairing toggle. |
| `BOND` | action | STRING-ONLY | Trigger bonding with a device. |
| `PAIR` | action | STRING-ONLY | Trigger pairing (distinct from `BOND`? unclear). |
| `PAIRED=` | broadcast | STRING-ONLY | Pairing-completed broadcast. |
| `PAIRREQ=` | broadcast | STRING-ONLY | Incoming pairing request broadcast (numeric-comparison/PIN prompt). |
| `LINK` / `LINK=` | query/broadcast | STRING-ONLY | Generic link-state query/broadcast. |
| `SDP` | action/query | STRING-ONLY | SDP (Service Discovery Protocol) query trigger — likely how blueware performs the AA-capability SDP-UUID check documented in `hal/bluetooth.h`. |
| `COD` | query | STRING-ONLY | Class of Device. |
| `BIND` | action | STRING-ONLY | Unclear — possibly binds a specific profile to a connection. |
| `TDL` | action | STRING-ONLY | Unclear, no context. |
| `STAT` | query | STRING-ONLY | Generic status query, unclear scope. |
| `FEATURE` | query | STRING-ONLY | Feature-capability query. |
| `PAGE` | action | STRING-ONLY | BT paging (connection-establishment inquiry), classic BT term. |
| `I2SCFG` | query/set | STRING-ONLY | I2S audio interface config — relevant if this module's audio path uses I2S rather than PCM/analog. |
| `AUDLOOP` | action | STRING-ONLY | Audio loopback test mode. |
| `SETFD` / `GETFD` | set/query | STRING-ONLY | Unclear abbreviation ("FD" — file descriptor? forward?). |
| `SETMP` / `GETMP` / `ADDMP` / `GETNP` | set/query | STRING-ONLY | Unclear — "MP"/"NP" abbreviation not decoded. |
| `MMU` | query/set | STRING-ONLY | Unclear. |
| `TPMODE` | query/set | STRING-ONLY | Unclear ("Test Point"/"Transparent" mode?). |
| `PROFILE` / `PROFILE=` | query/set | STRING-ONLY | Likely selects which BT profile set is active/advertised. |
| `H+K`, `I*H` | — | STRING-ONLY | Malformed-looking tokens, likely substrings of longer format strings caught by the extraction, not real standalone commands — listed for completeness, not meaningful on their own. |
| `CLOSEAT` | action | STRING-ONLY | Likely closes the AT-command session/port. |
| `RSPDATA` / `RSPSTR` / `SPDATA` | query | STRING-ONLY | Generic response-data helpers, unclear which command family they belong to. |

## HFP (Hands-Free Profile) — largest single group

| Command | Form | Status | Notes |
|---|---|---|---|
| `HFPCONN=<mac>` | set | TESTED | Initiates an HFP connection to a bonded device. `hal::connect_device()`. Real hardware: fails with `ERR002` when a connection already exists/was in progress — treat `ERR002` here as "benign, likely already connected," not a hard failure. |
| `HFPDISC` | action | STRING-ONLY | Disconnect current HFP link. |
| `HFPCHUP` | action | STRING-ONLY | Call hang-up. |
| `HFPANSW` | action | STRING-ONLY | Answer incoming call. |
| `HFPDIAL` | action | STRING-ONLY | Dial a number. |
| `HFPADTS` | action | STRING-ONLY | Unclear — possibly "audio transfer to/from phone". |
| `HFPMUTE` / `HFPMUTED=` | action/broadcast | STRING-ONLY | Mute mic during a call / mute-state broadcast. |
| `HFPMCAL` | action | STRING-ONLY | Unclear, possibly "missed call" related. |
| `HFPDTMF` | action | STRING-ONLY | Send DTMF tones during a call. |
| `HFPNREC` | query/set | STRING-ONLY | Noise-reduction/echo-cancellation toggle — matches the confirmed `HFP_NREC=3` AEC test-mode finding from earlier project work (`project_voice_processor_aec_finding` memory). |
| `HFPVR` | action | STRING-ONLY | Voice-recognition activation trigger. |
| `HFPBATT` / `HFPBATT=` | query/set | TESTED (query) | **Directly relevant to the AA 17.4+ battery-gate investigation** (see `project_aa17_4_battery_gate` memory) — `=` form reports a battery level to the connected phone over HFP; this project's own code never calls the `=` form. Query (`?`) tested, logged via `hal::diagnose_battery_reporting()`. |
| `HFPSIG` / `HFPSIG=` | query/broadcast | STRING-ONLY | Signal-strength query/broadcast. |
| `HFPINFO` | query | STRING-ONLY | General HFP info query. |
| `HFPTIME` / `HFPTIME=` | query/set | STRING-ONLY | **Clock-sync lead, investigated 2026-08-15 — likely dead for the same reason as `CCLK`.** `HFPTIME=` sits in a cluster of unmistakably unsolicited-broadcast commands (`HFPIBR=`, `HFPRING=`, `HFPAUDIO=`, `HFPSIG=`, `HFPROAM=`, `HFPBATT=`, `HFPMANU=`, `HFPHWVER=`, `HFPSWVER=`, `HFPNUM=`, `HFPNET=`, `HFPLOST=`, `HFPDROP=`) — all of which relay standard phone-reported HFP indicator values (manufacturer, network, roam status, etc.) as blueware receives them, not values blueware actively queries. `HFPTIME=` is almost certainly the same shape: an automatic relay of whatever clock value the phone volunteers over HFP, not a command this project should send. But per the AOSP source (`bta_ag_cmd.cc`, both `android-10.0.0_r30` and current `packages/modules/Bluetooth` master — checked directly via WebFetch), **Android's own phone-side HFP Audio Gateway has never implemented `AT+CCLK`, on any version**, so the phone-side data `HFPTIME=` would relay was never being sent in the first place on stock Android — this isn't a blueware or wiring gap, it's a platform-level absence upstream of blueware. **Caveat:** this project's own `hal::watch_bluetooth_broadcasts()` currently has only one registered observer (`+AAPDEV=` filtering in `main.cpp`) — no generic broadcast logger exists, so if `HFPTIME=` *has* ever fired on real hardware (e.g. from a non-stock ROM or OEM HFP stack that does implement CCLK), it would not have been visible in any captured log. Confirmed via repo-wide grep: `HFPTIME` has never appeared in any captured log. Not worth querying directly (bare `AT+HFPTIME` is untested and its semantics as an active query, if any, are unconfirmed) — if this lead is revisited, add it as a broadcast-observer case rather than an active query. |
| `HFPSTAT` / `HFPSTAT=` | query/broadcast | STRING-ONLY | HFP connection status query/broadcast. |
| `HFPSR=` | broadcast | STRING-ONLY | Unclear ("Signal Ring"?). |
| `HFPIBR=` | broadcast | STRING-ONLY | "In-Band Ringing" indicator, standard HFP concept. |
| `HFPRING=` | broadcast | STRING-ONLY | Incoming-call ring broadcast. |
| `HFPAUDIO=` | broadcast | STRING-ONLY | Audio-routing state broadcast (SCO connected/disconnected). |
| `HFPROAM=` | broadcast | STRING-ONLY | Roaming-status broadcast (standard HFP `+CIEV` roam indicator, relayed). |
| `HFPMANU=` | broadcast | STRING-ONLY | Phone manufacturer string broadcast (standard HFP `AT+CGMI` result, relayed). |
| `HFPHWVER=` / `HFPSWVER=` | broadcast | STRING-ONLY | Phone hardware/software version broadcasts. |
| `HFPNUM=` | broadcast | STRING-ONLY | Caller-ID number broadcast. |
| `HFPNET=` | broadcast | STRING-ONLY | Network-operator name broadcast (standard HFP `AT+COPS`, relayed). |
| `HFPLOST=` | broadcast | STRING-ONLY | Link-loss broadcast. |
| `HFPDROP=` | broadcast | STRING-ONLY | Call-dropped broadcast. |
| `HFPCFG` | query/set | STRING-ONLY | General HFP feature config. |
| `HFPDEV=` | broadcast | STRING-ONLY | Connected-device-identity broadcast (mirrors `+AAPDEV=`'s own confirmed shape). |
| `HFPCID=` | broadcast | STRING-ONLY | Call-ID broadcast (multi-call scenarios). |
| Standard relayed AT vocabulary: `BRSF`, `BSIR`, `BTRH`, `CCWA`, `CFM`, `CHLD`, `CHUP`, `CIEV`, `CIND`, `CLCC`, `CLIP`, `CMER`, `CNUM`, `COPS`, `CSCS`, `NREC`, `VGM`, `VGS`, `VTS`, `BAC`, `BCC`, `BCS`, `BLDN`, `XAPL` | — | STRING-ONLY | These are the standard 3GPP/Bluetooth SIG HFP AT-command vocabulary (call control, indicators, codec negotiation) that blueware relays between the phone and the AT-command port largely as-is — not blueware-specific extensions. Full semantics are the public HFP 1.6+/3GPP TS 27.007 spec, not re-derived here. |
| `CGMI` / `CGMM` / `CGMR` | query | STRING-ONLY | Standard 3GPP manufacturer/model/revision queries (relayed to/from phone). |
| `CCLK` / `CCLK?` | query | TESTED | Phone clock query, standard 3GPP `AT+CCLK`. **Fails with `ERR004` on every real hardware test this project has run — now confirmed structurally unfixable from the head-unit side.** Checked directly against AOSP's own Bluetooth Audio Gateway source (`bta_ag_cmd.cc`) at both `android-10.0.0_r30` and current `packages/modules/Bluetooth` master via WebFetch: `CCLK` is absent from both the HSP and HFP AT-command tables (`bta_ag_hsp_cmd[]`/`bta_ag_hfp_cmd[]`) in every version checked. Android's phone-side HFP stack has never answered `AT+CCLK` from a connected accessory — this is a platform-level gap, not a blueware/wiring/pairing problem, and not something any change on this project's side can fix. The `HFPTIME=` broadcast (see below) relays the same underlying data and is dead for the identical reason. See `project_aa_missing_auth_complete` memory follow-up #23-#25 for the full investigation; the CTS (`SYSCLKCFG`) lead remains the only clock-sync path not yet ruled out. |

## A2DP (Advanced Audio Distribution Profile)

| Command | Form | Status | Notes |
|---|---|---|---|
| `A2DPCONN` / `A2DPDISC` | action | STRING-ONLY | Connect/disconnect A2DP streaming. |
| `A2DPDEV=` | broadcast | STRING-ONLY | Connected-device broadcast. |
| `A2DPCODEC=` | query/set | STRING-ONLY | Codec selection (SBC/AAC/etc). |
| `A2DPMUTE` / `A2DPMUTED=` | action/broadcast | STRING-ONLY | Mute control/state. |
| `A2DPSR=` | broadcast | STRING-ONLY | Sample-rate broadcast. |
| `A2DPSTAT` / `A2DPSTAT=` | query/broadcast | STRING-ONLY | Connection status query/broadcast. |
| `A2DPLOST=` | broadcast | STRING-ONLY | Link-loss broadcast. |

## AVRCP (Audio/Video Remote Control Profile)

| Command | Form | Status | Notes |
|---|---|---|---|
| `AVRCPCFG` | query/set | STRING-ONLY | Feature config. |
| `AVRCPCONN` / `AVRCPDISC` | action | STRING-ONLY | Connect/disconnect. |
| `AVRCPSTAT` / `AVRCPSTAT=` | query/broadcast | STRING-ONLY | Status query/broadcast. |
| `PLAY` / `PAUSE` / `STOP` / `PLAYPAUSE` / `FORWARD` / `BACKWARD` / `REPEAT` / `SHUFFLE` | action | STRING-ONLY | Standard AVRCP media transport controls. |
| `PLAYSTAT=` / `PLAYMODE=` / `PLAYUID=` | broadcast/set | STRING-ONLY | Playback-state broadcasts / mode+track-UID set. |
| `TRACKINFO` / `TRACKINFO=` | query/broadcast | STRING-ONLY | Now-playing metadata query/broadcast. |
| `TRACKSTAT=` | broadcast | STRING-ONLY | Track-change broadcast. |
| `COVERART=` | broadcast | STRING-ONLY | Album-art data broadcast (AVRCP 1.6 cover-art feature). |
| `BROWDATA=` / `BROWSTAT=` | broadcast | STRING-ONLY | Media-browsing data/status broadcasts. |

## GATT / BLE

| Command | Form | Status | Notes |
|---|---|---|---|
| `GATTSTAT` / `GATTSTAT=` | query/set | TESTED (query) | GATT connection/service status. Tested via `hal::diagnose_battery_reporting()`, no CTS/Battery-Service-specific content confirmed in the response format. |
| `GATTSEND` | action | STRING-ONLY | Send GATT data. |
| `GATTDATA=` | broadcast | STRING-ONLY | Incoming GATT data broadcast. |
| `ADVDATA` / `ADVEN` / `ADVSTR` | query/set | STRING-ONLY | BLE advertising data/enable/string config. |
| `LEADDR` / `LENAME` | query | STRING-ONLY | BLE-specific address/name (may differ from classic BT `ADDR`/`NAME` on a dual-mode module). |
| `SYSCLKCFG` / `SYSCLKCFG?` | query/set | TESTED (query) | Clock-sync lead, now closed — see `project_aa_missing_auth_complete` follow-up #24/#25. No response content confirmed yet from real hardware, but the BLE Current Time Service (CTS) theory that motivated this query was ruled out by direct binary byte-search (CTS/Current-Time/Battery UUID hit counts all sit at or below random-noise baseline for this binary's size, and every hit disassembles as ordinary code, not a GATT table) — this command remains a harmless standing diagnostic, not an active lead. |

## AAP / IAP (Android Auto Projection / Apple iAP — wireless AA/CarPlay detection)

| Command | Form | Status | Notes |
|---|---|---|---|
| `AAPCONN` / `AAPDISC` | action | STRING-ONLY | Connect/disconnect the AA-projection-specific link. |
| `AAPDEV=` | broadcast | BROADCAST-SEEN | **Confirmed, load-bearing for this whole project** — real device-identity broadcast on genuine AA-capable-device SDP detection. See `project_aa_bluetooth_auto_start` memory for the full mechanism this project's auto-start feature is built on. |
| `AAPSTAT=` | broadcast | BROADCAST-SEEN | Confirmed live, cycles alongside `AAPDEV=` around one real detection event. |
| `IAPCONN` / `IAPDISC` | action | STRING-ONLY | Apple iAP (CarPlay-equivalent) connect/disconnect — this project's CarPlay work uses a different path (see `docs/ARCHITECTURE.md`'s carplay-sidecar section); unclear whether this AT-level path is actually exercised by anything in this codebase. |
| `IAPDEV=` / `IAPSTAT=` | broadcast | STRING-ONLY | iAP device/status broadcasts, mirrors the AAP shape. |
| `HICARDEV=` | broadcast | STRING-ONLY | "HiCar" (Huawei's AA/CarPlay-equivalent protocol) device broadcast — this module apparently supports detecting a third projection standard this project has never touched. |

## SPP (Serial Port Profile)

| Command | Form | Status | Notes |
|---|---|---|---|
| `SPPCONN` / `SPPDISC` | action | STRING-ONLY | Connect/disconnect a raw serial-over-BT link. |
| `SPPDEV=` | broadcast | STRING-ONLY | Connected-device broadcast. |
| `SPPDATA=` | broadcast | STRING-ONLY | Incoming SPP data broadcast. |
| `SPPSEND` | action | STRING-ONLY | Send SPP data. |
| `SPPSTAT` / `SPPSTAT=` | query/broadcast | STRING-ONLY | Status query/broadcast. |

## MAP (Message Access Profile)

| Command | Form | Status | Notes |
|---|---|---|---|
| `MAPDOWN` | action | STRING-ONLY | Download messages. |
| `MAPSEND` | action | STRING-ONLY | Send a message. |
| `MAPSTAT=` | broadcast | STRING-ONLY | Status broadcast. |
| `MSGNEW=` | broadcast | STRING-ONLY | New-message-arrived broadcast. |
| `MSGDATA=` / `MSGDATA=E,` | broadcast | STRING-ONLY | Message content broadcast (the `E,`-suffixed variant likely an encoding/escape marker). |
| `MSGSEND=` | set | STRING-ONLY | Message content to send. |
| `MSGSENT=E,` / `MSGSENT=F` | broadcast | STRING-ONLY | Send-result broadcasts (Error/Finished?). |

## PBAP (Phone Book Access Profile)

| Command | Form | Status | Notes |
|---|---|---|---|
| `PBABORT` | action | STRING-ONLY | Abort an in-progress phonebook transfer. |
| `PBCNT=` | broadcast | STRING-ONLY | Contact-count broadcast. |
| `PBDATA=` / `PBDATA=E` | broadcast | STRING-ONLY | Phonebook entry data broadcast. |
| `PBDISC` | action | STRING-ONLY | Disconnect PBAP. |
| `PBDOWN` | action | STRING-ONLY | Download phonebook. |
| `PBSEP` | query | STRING-ONLY | Supported-entry-parameters query. |
| `PBSTAT` / `PBSTAT=` | query/broadcast | STRING-ONLY | Status query/broadcast. |

## Other profiles (HID, BIP, FTP, PAN)

| Command | Form | Status | Notes |
|---|---|---|---|
| `HIDSTAT=` | broadcast | STRING-ONLY | HID (keyboard/media-key) profile status. |
| `BIPSTAT=` | broadcast | STRING-ONLY | Basic Imaging Profile status. |
| `FTPSTAT=` | broadcast | STRING-ONLY | File Transfer Profile status. |
| `PANSTAT=` | broadcast | STRING-ONLY | Personal Area Networking (Bluetooth tethering) status. |

## Misc / audio / uncategorized

| Command | Form | Status | Notes |
|---|---|---|---|
| `AUDROUTE=` | broadcast/set | STRING-ONLY | Audio-routing broadcast — matches the confirmed `AT+AUDROUTE=1/2` finding from earlier MCU work (this is the SoC/MCU's own use of the same-named concept, worth checking whether it's literally the same command surfaced here too). |
| `MICMUTE` / `MICMUTED=` | action/broadcast | STRING-ONLY | Mic mute control/state. |
| `SPKVOL` | query/set | STRING-ONLY | Speaker volume. |
| `PEERDATA=` | broadcast | STRING-ONLY | Generic peer-data broadcast, profile unclear. |
| `HC1STAT=` / `HC2STAT=` | broadcast | STRING-ONLY | Unclear — possibly dual-HCI-controller status on a module with two radios. |
| `GETMP`/`SETMP`/`ADDMP`/`GETNP` | — | STRING-ONLY | Already listed above under Core/system — cross-referenced here since they don't cleanly fit either category. |

---

## Summary: what's actually confirmed after this whole project

Of 209 total command tokens, **11 have been directly tested on real hardware** by this project (`BTEN`, `SCAN`, `PLIST`, `ADDR`, `NAME`, `PIN`, `HFPCONN`, `CCLK`, `HFPBATT` query, `GATTSTAT` query, `SYSCLKCFG` query), plus **4 confirmed as real unsolicited broadcasts** (`AAPDEV=`, `AAPSTAT=`, `DEVSTAT=`, `PWRSTAT=`). Everything else in this document is inferred from the command name and its position in the binary's string table — real, present, and dispatchable, but never exercised or verified by this project. Treat every "STRING-ONLY" entry as a lead for future investigation, not a confirmed fact.
