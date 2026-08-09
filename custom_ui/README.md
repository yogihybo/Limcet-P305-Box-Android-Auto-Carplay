# custom_ui — open-source replacement head-unit UI

Replacement for `MsnCoreApp` (the stock closed-source launcher/UI). Goal:
same functional coverage (CarPlay, Android Auto, settings, reversing
camera) with a fully open, customizable UI, running on the same
ArkMicro ARK1668 hardware this whole repo already understands.

## Why this shape

This hardware's display pipeline (DirectFB + Vivante GAL/galcore) has
been the single largest source of instability found across this entire
project — clock-stomp bugs, pin-sharing corruption, black-screen
regressions, all chased over many sessions just to keep the *existing*
app's picture stable. Betting a new UI on that same GPU-accelerated
compositing stack would inherit all of that risk on day one.

Instead:

- **UI toolkit: [LVGL v9](https://lvgl.io/)** (C, latest 9.x — not v8),
  rendering directly to `/dev/fb0` via its own Linux framebuffer driver
  — no DirectFB, no GPU/EGL dependency. CPU compositing only, by
  design. This is a mature, widely-used embedded GUI library,
  specifically built for hardware in this class (single core, tens of
  MB of RAM), and it's the toolkit of choice across the hobbyist
  car-headunit-replacement community for exactly this reason. v9
  specifically (over v8): the framebuffer and evdev input drivers are
  now built into core (`lv_linux_fbdev`/`lv_evdev`) rather than living
  in a separate `lv_drivers` repo, so there's one fewer dependency to
  vendor and keep in sync — and starting a new project on the version
  rolling into upstream maintenance-only status isn't worth it just for
  v8's larger stock of existing tutorials. GPU acceleration can be
  layered in later as an optimization (LVGL supports GPU-assisted
  blitting via a custom draw callback) once the CPU-rendered baseline
  is proven — it is not a prerequisite to getting a working UI on
  screen.
- **App logic: C++17.** Matches the vendor's own `sink`/`MsnCoreApp`
  pattern (our decompilation work carries over directly), zero-overhead
  interop with the C libraries this app actually has to talk to
  (ALSA, `libusb`, `ioctl()`s against `ark_display`/`hx170dec`), no
  second FFI boundary to maintain.
- **CarPlay: a bolt-on, not a dependency.** A separate sidecar process,
  `carplay-sidecar`, talks to the vendor's `sink` binary as a black
  box — `sink`'s MFi authentication certificate/private key are
  hardcoded into that binary (confirmed via decompilation, see
  `docs/ARCHITECTURE.md`) — licensed Apple credentials that can't
  legally be extracted or reproduced in a new binary, so `sink` keeps
  running exactly as it does today. `carplay-sidecar` is the only part
  of this project that links `libdbus` or knows anything about `sink`'s
  raw `com.arkmicro.auto` interface (4 methods — fully mapped, see
  below); it re-exposes a small local protocol over a Unix domain
  socket for the main UI to consume. This is a genuinely separate
  process, not just a separate source directory linked into the same
  binary, and that's deliberate on two counts: crash isolation (a
  `sink`/D-Bus hiccup can't take the UI down, and vice versa), and
  **implementation order** — per `docs/IMPLEMENTATION_PLAN.md`,
  everything else (Android Auto, settings, reversing camera, the actual
  UI-improvement work) gets built and shipped as a fully working
  replacement firmware first; CarPlay support is added afterward
  without touching the core app.
- **Android Auto: real upstream [`aasdk`](https://github.com/f1x/aasdk)**
  (vendored under `third_party/`) instead of the vendor's closed
  `libAndroidAuto.so`. Confirmed via decompilation that the vendor
  library uses the identical protobuf schema aasdk implements
  (`MediaSourceService`, `SensorSourceService`, `NavigationStatusService`,
  etc.) — same protocol, open implementation, no black-box dependency
  for this half.

See `docs/ARCHITECTURE.md` for the full hardware/IPC interface map this
plan is built on.

## Status

Scaffolding only. No app code yet. This directory structure and the
architecture doc are the starting point for actual implementation.

## Layout

```
custom_ui/
├── src/                    main UI binary
│   ├── main.cpp             entry point: fb init, LVGL tick, event loop
│   ├── core/                 app framework — screen manager, event bus
│   ├── ui/                    LVGL screens/widgets/themes (launcher, app switcher)
│   ├── androidauto/            aasdk integration layer
│   ├── settings/                settings screens + config file I/O
│   └── hal/                      hardware abstraction: fb0, ark_display
│                                 ioctls, ALSA, touch input, CAN/GPIO,
│                                 + the Unix-socket client for carplay-sidecar
├── sidecars/
│   └── carplay/             separate process, own libdbus dependency,
│       └── main.cpp          talks to sink's com.arkmicro.auto directly
├── include/                 public headers, if any end up needed
├── third_party/              vendored deps (lvgl, aasdk) — git submodules
├── assets/                   icons, fonts, theme resources
├── docs/
│   └── ARCHITECTURE.md       hardware + IPC interface reference
└── Makefile                  builds both custom_ui and carplay-sidecar
```

## Building

Not yet wired up — `Makefile` is a placeholder. Will use this repo's
existing cross toolchain the same way `linux-arkmicro`'s `env.source`
does (`arm-linux-gnueabihf-*`), targeting the same glibc-ABI concerns
already documented in `tools/nss-stub/README.md` (static linking +
NSS-stub treatment for any static binary touching `getpwnam`/`dlopen`/
etc. — the same class of host-toolchain-vs-target-glibc mismatch this
project has already solved once).
