# Architecture reference

Hardware/vendor interfaces this app talks to, kept here only where the
detail isn't already documented as source comments elsewhere in this
tree. For ioctl-level protocol detail on a given HAL module, read that
module's own header comment first (e.g. `src/hal/video_layer.cpp`,
`src/hal/camera.cpp`) — this doc doesn't repeat what's already there.

## Display

- Framebuffer: `/dev/fb0`, 800x480, RGBA888 (OSD1 layer).
- `/dev/ark_display` (misc device): ioctls for per-layer hue/saturation/
  brightness/contrast (`ARKDISP_GET_VDE_CFG`/`ARKDISP_SET_VDE_CFG`,
  layer_id 0-4) and screen info (`ARKDISP_GET_SCREEN_INFO`) — the hook
  for a future settings "Display" adjustment panel; not yet wired to a
  screen.
- Video decode: Hantro 8190 ASIC H.264 on `/dev/fb4`. See
  `src/hal/video_layer.cpp`'s own header comment for the real
  ioctl-level protocol (reverse-engineered from `sink`'s decompile).

## Touch & rotary knob

- Relayed by the Limcet MCU over `/dev/ttyHS0` (`hal::McuInputHal`).
- Rotary input in Android Auto: continuous rotation → `KEYCODE_NAVIGATE_
  NEXT/PREVIOUS` (261/260), center push → `KEYCODE_DPAD_CENTER` (23),
  hold-and-rotate → D-Pad card nudges (`KEYCODE_DPAD_RIGHT/LEFT` 22/21).

## Audio

ALSA, single card: `card0` (`ARK-SDDAC`).

## Bluetooth

Native Linux BlueZ 5.66 over kernel `hci0` (`rtk_hciattach` 3-wire UART
H5 @ 1.5 Mbps on `/dev/ttyHS1`, GPIO 91 reset). See
[`../../docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md`](../../docs/BLUEZ_AND_KERNEL_BLUETOOTH_HANDOFF.md)
for the D-Bus integration architecture.

## Reversing camera

`/dev/dvr` (ITU656/RN6752 decode pipeline: start/stop, channel select,
brightness/contrast/hue/mirror) and `/dev/carback` (app-ready
coordination + blocking reverse-gear-state notification) — both
optional at runtime, same non-fatal pattern as `hal::init_touch`.

Two real findings worth keeping, since neither is obvious from the
ioctl surface alone and both are load-bearing for how the UI has to
behave:

- **`ARK_DVR_GETFRAME` is a compiled-in no-op** in this driver — there
  is no software frame-readback path. The camera image is composited
  directly onto its own hardware display layer (`DISPLAY_LAYER=4`,
  separate from this app's LVGL/`fb0` GUI layer) by the LCDC itself,
  not delivered to userspace as pixel data. The HAL therefore only
  starts/stops the pipeline and adjusts image parameters — it cannot
  and does not pull frames into an LVGL canvas. The picture appears (or
  doesn't) independent of anything this process draws, as long as this
  app's own GUI layer isn't left opaque over the camera layer (see
  `hal::hide_display()`/`show_display()`).
- **The kernel driver has its own 500ms fallback**: `carback_int_work()`
  waits up to 500ms for `APP_ENTER_DONE`/`APP_EXIT_DONE` acks
  (`hal::ack_enter_done`/`ack_exit_done`) before forcibly hiding/showing
  the GUI layer itself. A missed ack degrades to an abrupt layer switch
  rather than a hang — worth knowing before treating a slow ack path as
  a real bug.

## CarPlay — via `sink` (kept as a black box)

`sink` (vendor binary, `firmware_dumps/Prado firmware dump/mtd6_rootfs/
usr/bin/sink`) owns the licensed Apple MFi authentication chain
(`Sink::getRootCert()`/`getClientCert()`/`getPrivateKey()` return
hardcoded constants compiled into the binary) — gated, paid-program
credential material that can't legally be extracted or reproduced, so
`sink` keeps running exactly as the vendor built it. `carplay-sidecar`
(`carplay/`) is a client of its D-Bus interface, nothing more.

- **Bus**: `sink` registers `com.arkmicro.auto` on the system bus
  (`ArkDbus::run()`).
- **Inbound methods** (4 total): `requestLinkStatus`,
  `requestTouchStatus`, `requestWheelStatus`, `requestKeyValue` — each
  dispatched to a `reply_to_*` handler.
- **Outbound**: at least one signal, `onLinkStatusChange(int, int)`.
- Full argument marshalling for these 5 calls hasn't been extracted
  (needs a decompile pass on the `reply_to_*` bodies) — the dispatch
  routing above is confirmed, the wire types aren't yet.
- **USB accessory detection** (`Accessory::isValid()`): matches any USB
  interface with class byte `0xFF` (vendor-specific) or `0x06`
  (still-image, Apple's MTP-mode signature) — no vendor/product ID
  check at all.

## CAN bus

`sink` has its own `CanComm` class (`init`/`send`/`registerCallbacks`),
separate from the stock app's `libCanBus.so`. Not yet investigated
which (if either) is authoritative on real hardware, or whether this
app needs CAN access at all for its scope (vehicle speed/reverse
signal, steering-wheel controls).

## Settings / config files

Real vendor config surface (`.ini`) is reverse-engineered in
`docs/1.10_SETTINGS_REFERENCE.md` (main repo) — full field reference
for `MsnProductInfo.ini`/`FactoryConfig.ini`. Live/provisioned settings
load from `/msnprofile/`; per-boot overridable state lives in
`/data/msncfg/Setting.config` once provisioned. This app's settings
screens should read/write that layer, not just the factory defaults.
