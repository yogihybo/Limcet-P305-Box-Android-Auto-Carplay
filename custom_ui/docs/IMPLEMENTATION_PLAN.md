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

- [ ] Implement this app's own `LinuxVideoSink`/`LinuxAudioSink`/
      `LinuxAudioSource`/`LinuxController`-equivalent classes against
      `aasdk`'s interfaces (naming/shape already confirmed via
      decompilation of the vendor's closed equivalent, see
      `ARCHITECTURE.md`)
- [ ] Wire video decode (`hx170dec`) and audio routing (ALSA)
- **Milestone**: an Android Auto session connects and displays through
  the new UI end to end.

## Phase 3 — Settings

- [ ] Display settings screen backed by `/dev/ark_display`
      (`ARKDISP_GET/SET_VDE_CFG`)
- [ ] Read/write live settings against `/data/msncfg/Setting.config`
      (the actual live layer, not just the static `.ini` factory
      defaults — see `project_language_setting_userdata` memory)
- [ ] Bluetooth / WiFi / volume screens
- **Milestone**: adjusting a setting in the new UI visibly changes
  device behaviour and survives a reboot.

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
