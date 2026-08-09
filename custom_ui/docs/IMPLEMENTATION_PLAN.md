# Implementation plan

Phased, each phase ending in something concretely demonstrable on real
hardware — not just "code exists." This project's own history
(`feedback_bootlog_evidence_weak`) is a hard-won reminder that a clean
build or a plausible code path is not confirmation; only an observed
result on the device counts. Same standard applies here.

**Design principle: CarPlay is a bolt-on, not a dependency.** Every
phase through Phase 6 (firmware integration) must produce a fully
functional replacement firmware — Android Auto, settings, reversing
camera, the actual "improve on stock" UI work — that works completely
with `carplay-sidecar` absent or not running. CarPlay support (Phase
7/8) gets added on top afterward, without requiring any changes to the
core app. This isn't just scheduling — the sidecar's whole reason for
existing as a separate process (see `README.md`/`ARCHITECTURE.md`) is
to make this decoupling real, not just a suggested order of work.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done +
hardware-confirmed.

## Phase 0 — Toolchain & vendoring

- [x] Vendor LVGL v9 under `third_party/lvgl` (git submodule, pinned to
      tag `v9.5.0`)
- [x] Vendor `aasdk` under `third_party/aasdk` (git submodule, pinned
      to tag `v4.1.20260426` — the actively-maintained `opencardev`
      fork; the original `f1x/aasdk` repo no longer exists)
- [x] Confirmed the real host-toolchain-vs-target-glibc mismatch this
      project already solved once for busybox
      (`tools/nss-stub/README.md`) applies here too: this repo's cross
      toolchain produces binaries needing `GLIBC_2.28`/`2.33`/`2.34`,
      target's real runtime glibc is `2.27`. Fixed by static-linking
      (`-static` in the `Makefile`, confirmed via `objdump` that the
      resulting binary has zero GLIBC symbol version references) — no
      `nss-stub` treatment needed on top, confirmed no NSS/dlopen
      warnings at static-link time.
- [x] Full LVGL v9 C source tree (~500 files, everything including
      optional widgets/backends) confirmed compiles clean end to end
      with the cross toolchain.
- [~] `aasdk` cross-compile validation not yet attempted — needs Boost
      resolved first (see Phase 2 below, task #6)
- **Milestone**: [x] code-complete, **not yet hardware-confirmed** — an
  LVGL v9 screen with one interactive button builds and static-links
  cleanly (`build/custom_ui`); needs the user to actually run it on the
  device against real `/dev/fb0`/`/dev/input/event0` to close this out.

(`libdbus` cross-build setup moves to Phase 7 now — it's only needed by
`carplay-sidecar`, which is deferred.)

## Phase 1 — HAL + core app skeleton

- [x] `src/hal/display.{h,cpp}`: framebuffer init via LVGL's
      `lv_linux_fbdev` driver against `/dev/fb0`
- [x] `src/hal/touch.{h,cpp}`: touch input via `lv_evdev` against
      `/dev/input/event0`, non-fatal if unavailable
- [x] `src/core/screen_manager.{h,cpp}`: minimal push/pop screen stack;
      `src/main.cpp` is now just wiring + the `lv_timer_handler()` loop
- [x] `src/ui/home_screen.{h,cpp}`: the Phase 0/1 test screen, now a
      proper screen factory instead of inline code in `main()`
- [x] Device-side test launcher, `scripts/run_on_device.sh` — **not**
      an `rcS` hook (that only takes effect after a reflash, which
      defeats "iterate without reflashing"); stops `MsnCoreApp` to free
      `/dev/fb0`, runs `custom_ui`, leaves the next real boot untouched
- **Milestone**: code-complete, structurally done (HAL/core/ui properly
  separated, confirmed still builds clean + fully static). **Still not
  yet hardware-confirmed** — same open item as Phase 0, needs the user
  to actually run it on the device.

## Phase 2 — Android Auto via aasdk

- [x] Resolve `aasdk`'s external dependency gaps (`find_package`s in
      its `CMakeLists.txt`): none of this repo's existing cross
      toolchains bundle headers for OpenSSL/libusb, and none bundle
      Boost at all; the target rootfs ships `libssl.so.1.1`/
      `libusb-1.0.so.0.1.0`/`libprotobuf.so.8.0.0` but no Boost, and
      linking against the target's own `.so`s was rejected (ABI
      gamble, reintroduces a glibc dependency). Decision: statically
      cross-compile all three from source, same pattern as everything
      else in this project that's had to cross the glibc-2.27 line.
  - [x] Boost **1.83.0** (not the newest release — pinned to match
        Ubuntu 24.04's `libboost-all-dev`, what aasdk's own CI actually
        builds against) — full build+install, not just
        `boost_log`+`boost_log_setup` (see script comment: a scoped
        build breaks `cmake --install`, which expects every configured
        library present) — `third_party/build_boost.sh`. **Correction**:
        an earlier attempt used 1.87.0, which compiled fine standalone
        but broke cross-compiling `aasdk` itself — aasdk's own source
        still uses `boost::asio::io_service`/`io_context::strand`
        directly, both fully removed from Boost.Asio by 1.87 (confirmed
        by hitting "'boost::asio::io_service' has not been declared").
        1.83 still has them.
  - [x] OpenSSL 1.1.1w (`libssl.a`+`libcrypto.a`) —
        `third_party/build_openssl.sh`
  - [x] libusb 1.0.29 (`libusb-1.0.a`, static, `--disable-udev` +
        netlink hotplug fallback) — `third_party/build_libusb.sh`
  - [x] Protobuf 25.3 + Abseil (`lts_2023_08_02`, pinned by
        protobuf's own `.gitmodules`) — **correction**: earlier notes
        here claimed this needed no separate work because aasdk
        "fetches Protobuf itself via `FetchContent`" — false, the only
        `FetchContent_Declare` anywhere in aasdk's CMake is for
        googletest. aasdk's `protobuf/CMakeLists.txt` (non-macOS
        branch) does a plain `find_path`/`find_library`/`find_program`
        for an already-installed protobuf and hard `FATAL_ERROR`s if
        missing. Needed two separate builds: a **host-native** `protoc`
        binary (Google's own prebuilt release binary, since a
        cross-compiled ARM `protoc` can't run on this build host) and
        a **cross-compiled ARM static** `libprotobuf` for target
        linking — `third_party/build_protobuf.sh`. The plain
        `protobuf-25.3.tar.gz` source release ships `third_party/
        abseil-cpp` as an empty submodule placeholder, not vendored
        source — the script clones it separately at the pinned tag.
  - All four verified as genuine ARM static archives (`file` on
    extracted `.o` members shows `ELF 32-bit LSB relocatable, ARM,
    EABI5`), not yet wired into `custom_ui/Makefile`.
- [x] Cross-compile `aasdk` itself against these four dependencies —
      `third_party/build_aasdk.sh`. `libaasdk.a` (74MB) +
      `libaap_protobuf.a` (263MB) built clean, confirmed genuine ARM
      static object code. Needed three fixes beyond the dependency
      gaps above, all now handled by the script:
      1. `add_library(aasdk SHARED ...)`/`add_library(aap_protobuf
         SHARED ...)` — both default to SHARED on non-macOS, patched
         to STATIC (`sed -i`, not upstreamed — aasdk's own Darwin-only
         STATIC branch suggests this was a deliberate narrow choice)
      2. `set(Boost_USE_STATIC_LIBS OFF)` — a plain (non-CACHE) `set()`
         in aasdk's `CMakeLists.txt` silently shadows the
         `-DBoost_USE_STATIC_LIBS=ON` command-line flag; patched the
         default directly, also dropped `-DBOOST_ALL_DYN_LINK`
      3. Protobuf variable case mismatch: `protobuf/CMakeLists.txt`'s
         manual `find_path`/`find_library` populate mixed-case
         `Protobuf_INCLUDE_DIR`/`Protobuf_LIBRARY`, but both
         `include_directories()`/`target_link_libraries()` calls (in
         that file and the top-level one) reference all-caps
         `PROTOBUF_INCLUDE_DIR`/`PROTOBUF_LIBRARIES` — a variable that
         was never actually set by that path. Fixed by passing the
         all-caps names explicitly as `-D` cache overrides so every
         scope agrees regardless of which spelling a given
         `CMakeLists.txt` line uses.
      Not yet wired into `custom_ui/Makefile`.
- [ ] Implement this app's own `LinuxVideoSink`/`LinuxAudioSink`/
      `LinuxAudioSource`/`LinuxController`-equivalent classes against
      `aasdk`'s interfaces (naming/shape already confirmed via
      decompilation of the vendor's closed equivalent, see
      `ARCHITECTURE.md`)
- [ ] Wire video decode (`hx170dec`) and audio routing (ALSA)
- **Milestone**: an Android Auto session connects and displays through
  the new UI end to end.

## Phase 3 — Settings

**Design principle: same options as stock, better format.** Not a
1:1 UI clone of stock's settings screens — a single unified config UI
covering the same functional options (nothing added, nothing silently
dropped), reorganized into **Basic** and **Advanced** tiers instead of
stock's flat/scattered menu structure:
- **Basic tier**: the handful of settings a normal daily user actually
  touches — language, volume/audio balance, display brightness/
  contrast, Bluetooth pairing, WiFi. Front and center, no digging.
- **Advanced tier**: everything else confirmed live in
  `docs/SETTINGS_REFERENCE.md`/`project_msnproductinfo_config_exploration`
  (CAN type, screen type, mirroring-link type, factory/diagnostic-ish
  fields) — one tap away behind an "Advanced" entry point, not deleted,
  not hidden entirely, just out of the way of the common path.
- Fields confirmed dead/no-op at runtime (per
  `project_msnproductinfo_config_exploration` memory — e.g.
  `MirroringLinkType` has no effect on this device, `ScreenType` gets
  overwritten by the MCU) are explicitly **not** reimplemented as if
  they were real; note them in `docs/SETTINGS_REFERENCE.md` instead if
  not already there.

- [ ] Replicate the settings module itself: a config-backed settings
      store mirroring stock's two-layer model — `FactoryConfig.ini`
      as one-time seed, `/data/msncfg/Setting.config` as the live,
      persisted layer actually read at runtime (see
      `project_language_setting_userdata` memory)
- [ ] `ui/settings`: single settings screen/menu with a Basic/Advanced
      tier switch, backed by the config store above — not a
      per-category screen tree like stock
- [ ] Display settings backed by `/dev/ark_display`
      (`ARKDISP_GET/SET_VDE_CFG`) — Basic tier
- [ ] Replacement Bluetooth menu: pairing/device-list UI, backed by
      whatever the real BT stack on this device is (need to confirm —
      likely BlueZ over the SoC's own BT/WiFi combo chip, not
      `sink`/CarPlay's own BT usage which is a separate concern) —
      device list, pair/connect/forget, connected-device status —
      Basic tier
- [ ] WiFi / volume screens — Basic tier
- [ ] Remaining `SETTINGS_REFERENCE.md` fields (CAN type, screen type,
      etc.) — Advanced tier
- **Milestone**: adjusting a setting in the new UI visibly changes
  device behaviour and survives a reboot; pairing a phone over the new
  Bluetooth menu results in a real paired/connected device; Basic vs.
  Advanced tiers are visibly distinct in the running UI.

## Phase 4 — Reversing camera

- [ ] Resolve the open question: does reversing-camera video go
      through `sink`'s own `ArkReverse`/`VideoDecoder::EnterBackCar()`
      path, or is it fully independent of the CarPlay/AA pipeline?
      (If it turns out to be `sink`-mediated, this may end up
      deferred alongside CarPlay rather than done here — check early.)
- [ ] Wire `/dev/dvr` (`ARK_DVR_*` ioctls) + reverse-gear GPIO event
      into a camera-preview screen
- **Milestone**: shifting into reverse automatically shows the camera
  feed through the new UI.

## Phase 5 — Launcher polish ("improve on stock" work)

- [ ] App switcher / home screen layout
- [ ] Theming pass — this is the actual visual-improvement deliverable
      the project exists for; no fixed scope here, iterate against
      real usage
- **Milestone**: subjective — a distinct, demonstrably nicer UI than
  stock, shown running on the device.

## Phase 6 — Firmware integration (CarPlay-less baseline)

- [ ] Wire `custom_ui` into `rcS` (replacing or made selectable
      alongside `MsnCoreApp`) — `carplay-sidecar` is not part of this
      image yet
- [ ] Bundle into `build_bootable_sdcard.sh` (or a new dedicated build
      script) so a full image can be built and flashed in one step
- **Milestone**: cold boot goes straight to the new UI, with Android
  Auto, settings, and reversing camera all working, on real hardware,
  without a serial cable or manual intervention. **This is the first
  point at which the replacement firmware is genuinely usable day to
  day** — everything from here on is additive.

## Phase 7 — CarPlay sidecar: link status roundtrip (bolt-on)

- [ ] Get `libdbus` headers/lib available for the cross build (source
      from target rootfs, or cross-build it) — only `carplay-sidecar`
      needs this
- [ ] Finish the open decompilation question from
      `ARCHITECTURE.md`: full argument marshalling for the 4
      `ArkDbus::reply_to_*_call` bodies and `onLinkStatusChange`, not
      just method names
- [ ] Design + document the sidecar's local Unix-socket protocol
      (start by mirroring the 4 D-Bus requests + 1 signal 1:1)
- [ ] `sidecars/carplay`: `libdbus` client connecting to `sink`'s
      `com.arkmicro.auto` service
- [ ] `sidecars/carplay`: Unix domain socket server
- [ ] `src/hal`: Unix domain socket client for the sidecar (must be
      written so the UI works fine if the socket simply isn't there —
      no sidecar running is a normal, supported state, not an error)
- **Milestone**: plug a phone into the existing `sink` binary, watch
  `carplay-sidecar` report the link-status change over the local
  socket, visible in the UI (a debug log line is enough for this
  milestone — video comes in Phase 8). Confirm the Phase 6 image still
  boots and runs fine with the sidecar binary simply absent.

## Phase 8 — CarPlay video/touch passthrough (bolt-on)

- [ ] Identify which display layer `sink`'s video decode path targets
      today (VIDEO2, per earlier project findings) and how to
      composite/window it under the new UI
- [ ] Touch event passthrough: UI → sidecar → `sink`
- [ ] Add `carplay-sidecar` into the firmware build alongside
      `custom_ui` (extends Phase 6's build, doesn't replace it)
- **Milestone**: a full CarPlay session, video and touch, working
  through the new UI end to end.

## Explicitly out of scope for now

- GPU-accelerated rendering (CPU compositing via LVGL is the baseline;
  revisit only if a specific screen's performance demands it)
- CAN bus integration (open question in `ARCHITECTURE.md` whether it's
  even needed for this scope)
- Any attempt to extract or reproduce `sink`'s embedded MFi
  credentials — not happening, full stop (see `ARCHITECTURE.md` and
  `README.md`)
