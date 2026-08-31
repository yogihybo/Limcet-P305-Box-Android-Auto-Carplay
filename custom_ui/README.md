# custom_ui — open-source replacement head-unit UI

Replacement for `MsnCoreApp` (the stock closed-source launcher/UI) for the
Limcet ARK1668-based Prado head unit: CarPlay, Android Auto, settings, and
reversing camera, with a fully open, customizable UI.

## Architecture

- **UI toolkit: [LVGL v9](https://lvgl.io/)** (C, v9.x), rendering directly
  to `/dev/fb0` via its own Linux framebuffer driver. CPU compositing only —
  no DirectFB, no GPU/EGL dependency.
- **App logic: C++17.**
- **Android Auto**: `micro_aap`, a from-scratch pure-C AAP protocol
  implementation (own dependency-free build, no Boost/Protobuf/OpenSSL
  submodule chain) — see `micro_aap/README.md` for its own architecture.
  Built as a standalone binary (`androidauto-sidecar`) and staged into
  `build/` automatically by the `ui`/`all` Makefile targets.
- **CarPlay**: a separate sidecar process, `carplay-sidecar` (`carplay/`),
  talking to the vendor's `sink` binary over D-Bus as a black box — `sink`
  owns the licensed Apple MFi authentication material and keeps running
  exactly as the vendor built it. `carplay-sidecar` is the only part of this
  project that links `libdbus` against `sink`'s `com.arkmicro.auto`
  interface; it re-exposes a small local protocol over a Unix domain socket
  for the main UI to consume.

See `docs/ARCHITECTURE.md` for the full hardware/IPC interface reference
this app is built against.

## Layout

```
custom_ui/
├── src/                     main UI binary (`ui` target)
│   ├── main.cpp               entry point: fb init, LVGL tick, event loop
│   ├── core/                   app framework — screen manager, navigation,
│   │                             config store, threading
│   ├── ui/                      LVGL screens/widgets/themes
│   └── hal/                      hardware abstraction: fb0/ark_display
│                                 ioctls, ALSA, MCU serial, BlueZ D-Bus,
│                                 Android Auto client socket
├── carplay/                 carplay-sidecar: own libdbus dependency,
│   └── main.cpp                talks to sink's com.arkmicro.auto directly
├── micro_aap/                androidauto-sidecar: pure-C AAP protocol
│                               implementation, own Makefile/README
├── third_party/               vendored deps (lvgl, nanopb, bluez_uapi)
├── ladspa_eq/                 system-wide EQ/loudness LADSPA plugin
├── etc/                       runtime config staged into build/ (ALSA,
│                               hal.conf, default_settings.conf)
├── assets/                    fonts
├── scripts/                   device deploy/diagnostic helpers
├── docs/
│   ├── ARCHITECTURE.md          hardware + IPC interface reference
│   ├── IMPLEMENTATION_PLAN.md   remaining work checklist
│   └── UI_REDESIGN_PROPOSAL.md  Material 3 / Coolwalk screen spec
└── Makefile                   builds ui, carplay-sidecar,
                                androidauto-sidecar, ladspa-eq
```

## Building

Cross-compiles against this repo's Buildroot toolchain
(`linux-arkmicro/buildroot/output/host/bin/arm-buildroot-linux-gnueabihf-*`,
overridable via `BUILDROOT_OUTPUT_DIR`).

```sh
make ui              # custom_ui + androidauto-sidecar, staged into build/
make carplay-sidecar  # carplay-sidecar
make ladspa-eq        # system EQ/loudness LADSPA plugin
make all              # everything above
make clean
```

`make ui` always leaves `build/custom_ui` and `build/androidauto-sidecar`
current — it recurses into `micro_aap`'s own build and re-stages its output
as part of the same invocation, so there's no separate step to remember.
