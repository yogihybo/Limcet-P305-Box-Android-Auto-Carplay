# Architecture reference

Working notes on the hardware/IPC surfaces this app needs to talk to.
Everything here is either decompiled/confirmed from the real vendor
binaries or hardware-confirmed elsewhere in this repo — cross-referenced
where relevant. This doc is meant to grow as implementation proceeds;
treat it as the source of truth for "what does the hardware/vendor
stack actually expose," not a design spec.

## Display

- Framebuffer: `/dev/fb0`, 800x480, RGBA888 (OSD1 layer). Kernel driver
  is `linux-arkmicro/linux/drivers/video/fbdev/arkmicro/ark1668_lcdfb.c`
  in the sibling `linux-arkmicro` repo — this project's own
  reconstruction, hardware-confirmed stable as of the 2026-08-09 fixes
  (bootlogo-address unification, lcdclk early-boot divider-stomp fix,
  i2c-gpio-1 pin-conflict elimination).
- `/dev/ark_display` (misc device, `drivers/misc/ark_display.c`):
  ioctls for per-layer hue/saturation/brightness/contrast
  (`ARKDISP_GET_VDE_CFG`/`ARKDISP_SET_VDE_CFG`, layer_id 0-4) and
  screen info (`ARKDISP_GET_SCREEN_INFO`). This is the settings screen's
  hook for a "Display" adjustment panel.
- GPU (Vivante GAL/galcore): available if needed later, but the LVGL
  baseline deliberately does not depend on it. See
  `linux-arkmicro/gpu/known-good-pairing-6.2.4.p1.8/README.md` for the
  known-good `galcore.ko`/`libGAL.so` pairing if GPU-accelerated
  blitting is ever added.

## Touch input

`/dev/input/eventN`, standard Linux evdev (ARK1680 resistive touch
controller, built into the kernel). The stock app's Qt/QWS backend
needed `QWS_MOUSE_PROTO`/`QWS_ARK_MT_DEVICE` env-var gating
(`project_touch_qws_env_gate`) — that's a Qt/QWS-specific requirement,
not a hardware one. LVGL reads evdev directly (`lv_indev` + a plain
`read()` loop against the event device), no equivalent gating needed.

## Audio

ALSA, single card: `card0` (`ARK-SDDAC`). No mixer/routing surprises
documented elsewhere in this repo as of this writing.

## CarPlay — via `sink` (kept as a black box)

Decompiled from the real vendor `sink` binary
(`firmware_dumps/Prado firmware dump/mtd6_rootfs/usr/bin/sink`,
not stripped, full C++ symbols) on 2026-08-09.

**D-Bus service**: `sink` registers as bus name `com.arkmicro.auto` on
the **system** bus (`ArkDbus::run()`, confirmed via decompile — plain
`libdbus` connect/request-name/read-write loop). Exactly 4 inbound
method calls are dispatched:

| Method | Handler |
|---|---|
| `requestLinkStatus` | `ArkDbus::reply_to_requestLinkStatus_call` |
| `requestTouchStatus` | `ArkDbus::reply_to_requestTouchStatus_call` |
| `requestWheelStatus` | `ArkDbus::reply_to_requestWheelStatus_call` |
| `requestKeyValue` | `ArkDbus::reply_to_requestKeyValue_call` |

Plus at least one outbound signal/notification path,
`ArkDbus::onLinkStatusChange(int, int)`. Exact D-Bus interface name and
full argument signatures for each method are not yet extracted (next
step — decompile the 4 `reply_to_*` bodies and the `onLinkStatusChange`
call site to get argument types/marshalling, not just the dispatch
routing).

**What this app needs to build**: `sidecars/carplay/`, a standalone
process (own `libdbus` dependency, isolated from the main UI binary —
see README for why) that calls these 4 methods and listens for the
status signal, translating between `sink`'s wire format and a small
local protocol exposed over a Unix domain socket. The main UI's
`src/hal/` gets a thin client for that socket — no `libdbus`, no
knowledge of `sink`'s interface, anywhere in the UI binary. Exact
local protocol (framing, message set) not yet designed — first pass
should probably just mirror the 4 requests + 1 signal 1:1 before adding
any UI-specific abstraction on top.

**Why `sink` itself isn't being replaced**: `Sink::getRootCert()`,
`Sink::getClientCert()`, and `Sink::getPrivateKey()` all return
hardcoded string constants compiled directly into the binary — almost
certainly the OEM's licensed Apple MFi authentication certificate
chain. That's gated, paid-program credential material; it can't be
legally extracted and reused in a new, uncertified binary. `sink` stays
running exactly as the vendor built it; this app is a client of its
D-Bus interface, nothing more.

**USB accessory detection** (`Accessory::isValid()`, decompiled):
matches any USB interface with class byte `0xFF` (vendor-specific) or
`0x06` (still-image, Apple's MTP-mode signature) — no vendor/product ID
check at all. Matches this project's own earlier finding
(`project_wired_aa_aoa_investigation` memory) exactly; decompilation
output is trustworthy.

## Android Auto — via upstream `aasdk`

`libAndroidAuto.so` (vendor, closed, stripped) contains protobuf
message classes (`MediaSourceService`, `SensorSourceService`,
`NavigationStatusService`, etc.) matching the real Android Auto wire
protocol schema — the same one
[`aasdk`](https://github.com/f1x/aasdk) (used by OpenAuto/Crankshaft)
already implements openly. `sink`'s own callback interface classes
(`LinuxVideoSink`, `LinuxAudioSink`/`LinuxAudioSource`,
`LinuxController` with `pingRequestCallback`/`byeByeRequestCallback`/
`serviceDiscoveryRequestCallback`/`voiceSessionNotificationCallback`)
use aasdk's own naming convention directly, reinforcing this.

Plan: vendor `aasdk` under `third_party/`, implement this app's own
video/audio/input sink classes against its interfaces directly — no
dependency on the vendor's `libAndroidAuto.so` at all for this half.

## Wireless AA discovery — via `blueware`'s `/dev/bw_aap` socket (confirmed from real traffic)

This device has **no standard Linux BlueZ stack** — no `bluetoothd`, no
`libbluetooth.so`, no `hciconfig`/`hcitool`/`sdptool` anywhere in the
rootfs (checked directly). Bluetooth is Feasycom's own closed
`blueware` daemon (`/usr/bin/blueware`, talks to the BT chip over UART
at `/dev/ttyHS1`, config at `/etc/blueware-bw*.properties` — see
`docs/logs/bluetooth log stock_260718.txt`), which implements HCI/
RFCOMM/SDP entirely in userspace and exposes app-facing local Unix
domain sockets instead of kernel `AF_BLUETOOTH` sockets — `/dev/bw_iap`
for iAP2 (CarPlay) and **`/dev/bw_aap` for AAP (Android Auto
Protocol)**.

Confirmed from real captured traffic — `docs/logs/android auto log
v{1,2,3}.txt`, stock `sink` binary, class `BtRfcommController` — a
small, separate pre-connection protobuf schema
(`aap_protobuf::aaw::*`, vendored in `aasdk`'s `protobuf/aap_protobuf/
aaw/` — "aaw" = Android Auto Wireless) is exchanged directly over
`/dev/bw_aap`, length/type-framed:

```
[uint16 length, big-endian][uint16 type, big-endian][protobuf payload]
```

`type` matches `aap_protobuf::aaw::MessageId`: `WIFI_START_REQUEST=1`,
`WIFI_INFO_REQUEST=2`, `WIFI_INFO_RESPONSE=3`, `WIFI_VERSION_REQUEST=4`,
`WIFI_VERSION_RESPONSE=5`. Real observed sequence:

1. HU → phone: `WIFI_VERSION_REQUEST` (type 4) — **note**: the
   vendored `.proto` declares this message empty
   (`message WifiVersionRequest {}`), but the real captured frame has
   a 9-byte payload — the vendored proto is evidently incomplete here,
   so this is replayed as known-good raw bytes, not constructed from
   the proto (see `src/androidauto/bw_aap_client.cpp`)
2. phone → HU: `WIFI_VERSION_RESPONSE` (type 5) — `sink`'s own log
   dump shows `majorVer`/`minorVer`/`deviceSerial`/`status`/`channel`
   fields, which also doesn't cleanly match the vendored proto's 4
   anonymous `unknown_value_*` fields — logged raw, not parsed yet
3. HU → phone: `WIFI_START_REQUEST` (type 1, `ip_address`/`port`) —
   **this proto is clean and confirmed field-for-field** against the
   capture; this is the head unit telling the phone where to connect
   (its own local WiFi AP address + a port)
4. phone → HU: `WIFI_INFO_REQUEST` (type 2, empty — matches its proto
   exactly, genuinely has no fields)
5. HU → phone: `WIFI_INFO_RESPONSE` (type 3, `ssid`/`password`/
   `bssid`/`security_mode`) — **also confirmed field-for-field**
   against the capture (`ssid="carplay_fc9f"`,
   `password="88888888"`, `bssid="68:b9:d3:f1:5a:43"` all decode
   exactly from the raw bytes)

After this, the real Android Auto session (aasdk's `Messenger`/
`Cryptor`/`ControlServiceChannel` — the stuff `Session` already
implements, see `src/androidauto/session.h`) runs over plain TCP to
whatever IP:port was exchanged — this pre-connection dance is a
lightweight bootstrap entirely separate from the encrypted aasdk
protocol, not something that goes through aasdk's `Messenger` at all.

`src/androidauto/bw_aap_client.{h,cpp}` implements all 5 steps above
(`startHandshake()` for 1-3, `respondToInfoRequest()` for 4-5).
**Not yet hardware-tested** — this is a faithful
reconstruction of real captured stock traffic, not confirmed to work
when driven by our own code. Also unconfirmed: whether `/dev/bw_aap`
accepts more than one simultaneous client (if `sink`/`MsnCoreApp`
already holds it open, our own connection may fail or interfere).

This **supersedes** an earlier, wrong-assumption approach
(`src/androidauto/bluetooth_transport.h` /
`bluetooth_rfcomm_server.h`, a raw `AF_BLUETOOTH`/`BTPROTO_RFCOMM`
kernel-socket transport + our own SDP server) built before this
traffic capture was checked — kept in the tree as legitimate generic
code, but not the path forward for this device.

## Reversing camera

- `/dev/dvr` (ITU656/rn6752 driver stack, this project's own kernel
  reconstruction) — `ARK_DVR_*` ioctls for camera preview.
- Reverse-gear detection: GPIO-based, `ark-carback` kernel driver.
- `sink` itself also has an `ArkReverse` class
  (`init`/`run`/`uninit`/`watchHandleIsExist`) and `VideoDecoder::
  EnterBackCar()`/`ExitBackCar()` — worth checking whether reversing
  camera display goes through `sink`'s own video pipeline or is fully
  independent; not yet investigated.

## CAN bus

`sink` has its own `CanComm` class (`init`/`send`/`registerCallbacks`)
— separate from the stock app's `libCanBus.so`. Not yet investigated
which one (if either) is authoritative on real hardware, or whether
this app needs CAN access at all for its scope (vehicle speed/reverse
signal, steering-wheel controls).

## Settings / config files

Real vendor config surface, all plain `.ini`, already reverse-engineered
in `docs/SETTINGS_REFERENCE.md` (main repo) — full field reference for
`MsnProductInfo.ini` and `FactoryConfig.ini`. Live/provisioned settings
(vs factory defaults) load from `/msnprofile/`; the actual per-boot
overridable state additionally lives in `/data/msncfg/Setting.config`
once provisioned (see `project_language_setting_userdata` memory) —
this app's settings screens should read/write that layer, not just the
static `.ini` factory defaults.

## Open questions / next steps

1. Full argument marshalling for `ArkDbus`'s 4 D-Bus methods (types,
   not just names) — needs one more decompile pass on the `reply_to_*`
   bodies.
2. Whether `com.arkmicro.auto` is *also* the interface name, or just
   the bus name (D-Bus separates these) — needed to construct correct
   method-call messages.
3. `aasdk` cross-compile validation on this toolchain/target glibc
   before committing to it as the AA backend.
4. Reversing-camera video path: confirm whether it's `sink`-mediated or
   independent of the CarPlay/AA pipeline.
