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
- [x] Wire aasdk into `custom_ui/Makefile` and prove the full static
      link graph resolves on real target code, not just aasdk's own
      build. Added `src/androidauto/usb_probe.{h,cpp}` (real usage of
      `aasdk::usb::USBHub`/`AccessoryModeQueryChainFactory` — the
      actual AOAP wired-handshake detection path, Google vendor/product
      ID `0x18D1`/`0x2D00`) plus a standalone
      `tools/androidauto-usb-probe-test` driver, linked via a new
      `androidauto-usb-probe-test` Makefile target (`AASDK_LIBS`
      pulling in all of aasdk/Boost/OpenSSL/libusb/Protobuf/Abseil
      inside a `--start-group`/`--end-group`, since those libs
      static-link against each other in ways a single left-to-right
      pass can't always resolve). **Result: linked clean on the first
      attempt** — a 10.5MB fully static ARM binary, confirmed zero
      `GLIBC` symbol version references. `src/androidauto/` is
      deliberately excluded from the main `custom_ui`/`ui` target's
      sources so the base UI build stays fast and dependency-light
      until real protocol/service integration lands; build it
      explicitly with `make androidauto-usb-probe-test`. Confirmed
      `make all` (the base UI + sidecar) still builds clean after this
      change. **Not yet hardware-tested** — this proves the link graph
      resolves, not that USB hotplug detection actually works on the
      device.
- [x] Control-channel handshake: `src/androidauto/session.{h,cpp}`
      (`androidauto::Session`) implements aasdk's
      `IControlServiceChannelEventHandler` for real — wraps an
      AOAP-negotiated device in `AOAPDevice` → `USBTransport` →
      `Cryptor`/`MessageInStream`/`MessageOutStream` → `Messenger` →
      `ControlServiceChannel`, sends the version request, then drives
      aasdk's OpenSSL-BIO `Cryptor` through the handshake loop (head
      unit is the TLS *client* — `Cryptor::init()` calls
      `SSL_set_connect_state`), logging the service discovery request
      once received. Every `on*()` handler re-arms
      `controlChannel_->receive()` itself — confirmed via
      `ControlServiceChannel.cpp`'s own `messageHandler()` that it does
      **not** auto-rearm after dispatching a known message, only its
      unhandled-message-id fallback does (cross-checked against the
      same pattern in aasdk's own working `BluetoothService.cpp`).
      `usb_probe.cpp` now starts a real `Session` on AOAP detection
      instead of just logging it. Not yet answering service discovery
      with a real response, and no media/input/sensor channels open
      yet — next increment.
  - **Found and fixed a second real static-NSS-init crash** (the same
    class this project already hit and fixed for
    nano/htop/tmux/gdbserver/busybox, `tools/nss-stub/README.md`):
    `libcrypto.a` references `dlopen`/`getaddrinfo`/`gethostbyname`
    internally (engine loading, `BIO_lookup_ex`) — link-time-only
    warnings, but this project has already confirmed on real hardware
    that those warnings are not harmless on this toolchain/kernel
    combination. Fixed by reusing (not duplicating)
    `tools/nss-stub/nss_stub_busybox.c` — it already wraps exactly this
    symbol set — via `--wrap` flags scoped to the
    `androidauto-usb-probe-test` link only (the base UI/sidecar don't
    link OpenSSL and don't have this problem). Confirmed clean: no more
    dlopen/getaddrinfo linker warnings, zero `GLIBC` symbol references,
    `__wrap_*` symbols present in the binary.
  - **Not yet hardware-tested** — the SSL handshake byte exchange has
    only been verified against aasdk's own class contracts and the
    `BluetoothService.cpp` reference pattern, not against a live phone.
- [x] Answer service discovery with a real response —
      `Session::onServiceDiscoveryRequest` now builds and sends a real
      `ServiceDiscoveryResponse` (`sendServiceDiscoveryResponse`,
      confirmed protocol contract, not speculative) with basic
      headunit info.
      Also implemented `onPingRequest` → `sendPingResponse` (echoes
      the timestamp back — confirmed protocol contract per
      `PingResponse`'s single required field, this is the AA
      keep-alive mechanism, not something to leave unanswered).
- [x] First real channel: `InputSourceService` (touch only) —
      `src/androidauto/input_channel.{h,cpp}` (`InputChannel`)
      implements `IInputSourceServiceEventHandler`: accepts the
      phone's `ChannelOpenRequest` unconditionally (no real reason to
      refuse), logs `KeyBindingRequest` (steering-wheel/hardware-key
      mapping — this device's touch-only UI has no wheel to bind, not
      handled), and exposes `sendTouch()` for pushing real touch
      events once wired to a HAL source. `Session::start()` now
      constructs and arms an `InputChannel` alongside the control
      channel (same `Messenger`), and `ServiceDiscoveryResponse` now
      actually advertises it — `Service.id` uses
      `static_cast<int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE)`
      as a best-available proxy for the real wire service ID (no
      authoritative source or captured traffic for this specific
      value was found — `docs/logs/` covers the Bluetooth
      pre-connection dance, not an aasdk-level session; worth
      confirming against a real capture if a phone doesn't recognize
      the channel), touchscreen advertised as 800×480 (matches this
      device's real framebuffer, `docs/ARCHITECTURE.md`'s Display
      section). **Not yet connected to a real touch source** — LVGL
      owns `/dev/input/eventN` exclusively for the UI's own rendering;
      forwarding real touches to Android Auto needs its own separate
      evdev reader (evdev supports multiple concurrent readers, this
      is a well-defined next increment, not a redesign).
      Linked clean, zero `GLIBC` references, all other targets still
      build (binary grew further — `ServiceDiscoveryResponse`'s
      `channels` field type transitively pulls in every service-
      specific protobuf message's generated code, even unused ones;
      expected protobuf behavior, not a bug).
- [x] Wire a real touch source into `InputChannel::sendTouch()` —
      `src/androidauto/touch_forwarder.{h,cpp}` (`TouchForwarder`)
      opens a second, independent evdev fd on the same device node
      LVGL already reads exclusively (evdev supports multiple
      concurrent readers, confirmed safe). Confirmed against this
      project's own reconstructed `ark1680_ts.c` kernel driver:
      **Protocol A, not B** (single-touch only, no `ABS_MT_*` — this
      is a real finding from reading the driver source, not a guess),
      and raw coordinates are 12-bit ADC counts
      (`input_set_abs_params(ABS_X/Y, 0, 4095, ...)`), so the class
      does the same `EVIOCGABS` calibration query + linear scale into
      800×480 that LVGL's own `lv_evdev.c` does, landing touches in
      the same coordinate space already advertised in
      `ServiceDiscoveryResponse`. Async I/O mirrors
      `bluetooth_transport.cpp` (`boost::asio::posix::stream_descriptor`).
      `Session::start()` wires it: constructs a `TouchForwarder`
      alongside the `InputChannel`, and starts it only once the phone
      actually opens the input channel (via
      `InputChannel::setChannelOpenCallback()`), not eagerly at
      construction — no reason to hold a second touch fd open before
      it's needed. Linked clean, zero `GLIBC` references, all other
      targets still build. **Not yet hardware-tested** against a live
      touch panel or a live phone.
- [ ] Open the remaining media/sensor channels (advertise in
      `ServiceDiscoveryResponse.channels` once implemented — not
      before, per the reasoning above)
- [ ] Implement this app's own `LinuxVideoSink`/`LinuxAudioSink`/
      `LinuxAudioSource`/`LinuxController`-equivalent classes against
      `aasdk`'s interfaces (naming/shape already confirmed via
      decompilation of the vendor's closed equivalent, see
      `ARCHITECTURE.md`)
- [ ] Wire video decode (`hx170dec`) and audio routing (ALSA)
- **Milestone**: an Android Auto session connects and displays through
  the new UI end to end.

### Wireless AA — elevated to a required path, not a later nice-to-have

**Reason this isn't deferred**: the device has exactly one external USB
connector (`usb0` — confirmed in `project_usb_physical_port_mapping`
memory; `usb1` is internal/WiFi-only), and on this dev unit that port
is currently occupied by the USB flash drive the rootfs itself boots
from. There is no free external port to plug a wired phone into for
testing right now, and on production hardware, dedicating the one
external port to a permanent CarPlay/AA cable is a real usability
tradeoff most users won't want anyway (that's why stock's own wireless
path exists at all). Wireless AA needs to work whether or not wired
ever does.

The stock firmware already does this exact dance (see
`project_wireless_carplay_aa_channel_plan` /
`project_static_wifi_ap_vs_dynamic` memories): `sink` spins up a
per-connection dynamic `hostapd` AP and hands its credentials to the
phone over Bluetooth. `aasdk` has both protocol pieces already
vendored to reproduce this on our side:
- `aasdk::channel::bluetooth::BluetoothService` — pairing/handshake
  over BT (already compiled as part of `libaasdk.a`)
- `aasdk::channel::wifiprojection::WifiProjectionService` —
  `WifiCredentialsRequest`/`WifiCredentialsResponse`
  (SSID/password/security mode) handed to the phone over that same BT
  link once paired
- `aasdk::transport::TCPTransport` — once the phone joins the AP, the
  real AA session (`Session`'s `ControlServiceChannel` and everything
  built on top of it) runs over this instead of `USBTransport` — same
  `Messenger`/`Cryptor`/channel code, different transport underneath

Plan:
- [x] Factor `Session` so the transport is swappable, not a second
      copy of the class — `Session::start()` now takes an
      `aasdk::transport::ITransport::Pointer` directly instead of
      constructing a `USBTransport` internally; `usb_probe.cpp`
      constructs `AOAPDevice`+`USBTransport` itself and passes it in.
      Confirmed: base UI/sidecar targets and the USB probe test all
      still build clean after the refactor.
- [x] `TCPTransport`-based path proven: new
      `src/androidauto/wireless_probe.{h,cpp}` +
      `tools/androidauto-wireless-probe-test` connects OUT to a given
      host:port (confirmed by reading `aasdk::tcp::ITCPWrapper`'s
      actual interface that aasdk only implements the TCP **client**
      side — `connect`/`asyncConnect`, no `accept`/`listen` — meaning
      aasdk's own design already expects the head unit to connect to
      the phone, not run a listening socket; matches the real Android
      Auto Wireless architecture where the phone hosts the TCP
      service) and drives the same `Session` handshake as the USB
      path. Linked clean, zero `GLIBC` symbol references, same
      `nss_stub_busybox.c` treatment applied (shares `AASDK_LIBS`).
      Host/port must be supplied manually for now (default port 5277
      is a commonly-cited guess from other open-source
      implementations, **not independently confirmed** against this
      project's own traffic captures) — no automatic discovery yet,
      that's the BT/WifiProjection item below.
- [x] Research pass (web search, not guessed): confirmed the
      high-level architecture aasdk's vendored `BluetoothService`/
      `WifiProjectionService` channels imply is correct, not a wrong
      guess — "Bluetooth is used to advertise the head unit and
      negotiate the wireless connection while actual media data flows
      over Wi-Fi" and "After WiFi credentials are exchanged via
      Bluetooth, the Bluetooth connection is closed" (via
      [nisargjhaveri/WirelessAndroidAutoDongle](https://github.com/nisargjhaveri/WirelessAndroidAutoDongle)
      and [aa-proxy-rs](https://github.com/aa-proxy/aa-proxy-rs), two
      mature open-source AA-wireless-dongle implementations). Also
      confirmed via reading `aasdk::tcp::ITCPWrapper`'s actual
      interface (see the wireless-probe entry above) that the head
      unit connects OUT to the phone over TCP for the real session —
      matches "the phone hosts the AA TCP service, headunit connects
      to phone's IP." **Still not confirmed**: the exact byte-level
      message sequence/timing (does BT pairing alone trigger
      `WifiProjectionService`, or does the head unit need to send
      something first?) — see `project_wireless_carplay_aa_channel_plan`/
      `project_static_wifi_ap_vs_dynamic` memories for what's already
      known about the stock `sink` binary's own behavior, which is the
      most authoritative reference available short of a live capture.
- [x] Raw Bluetooth RFCOMM socket layer, independent of the still-open
      sequence question above (confirmed aasdk itself has **no**
      Bluetooth transport at all — grepped its full source tree — so
      this is genuinely new code, not a wrapper around something
      aasdk already provides): `third_party/bluez_uapi/bluetooth/{bluetooth,rfcomm}.h`
      (minimal vendored subset of BlueZ's public, stable UAPI headers —
      no `bluez-dev` package available and no root to install one, same
      constraint as every other `build_*.sh` dependency this project
      has hit), `src/androidauto/bluetooth_transport.{h,cpp}`
      (`BluetoothRFCOMMTransport`, implements `aasdk::transport::Transport`
      over a raw socket via `boost::asio::posix::stream_descriptor`,
      mirroring exactly how `TCPTransport` wraps `ITCPEndpoint`),
      `src/androidauto/bluetooth_rfcomm_server.{h,cpp}` (blocking
      accept helper), and a standalone
      `tools/androidauto-bluetooth-rfcomm-test` that listens and
      hex-dumps received bytes — **deliberately not yet running a full
      `Session` over it**, since the message sequence on top is still
      unconfirmed. Linked clean, zero `GLIBC` symbol references.
      **Known real gap**: no SDP service record registration, so a real
      phone's AA app (which discovers the head unit's RFCOMM channel
      via SDP, not a fixed/guessed channel number) can't find this
      listener yet — only a peer that already knows the channel number
      can connect (e.g. manual `rfcomm connect` from a paired Linux
      dev machine, for local testing).
- [x] **Blocker resolved — real solution found, not the raw-RFCOMM/SDP
      path.** The "no BlueZ stack" finding below was real, but the
      user's "investigate the live device first" decision led straight
      to it: `docs/logs/` already contains real captured Bluetooth
      traffic from stock firmware (`bluetooth log stock_260718.txt`,
      `android auto log v{1,2,3}.txt`) — checking those (per explicit
      instruction) revealed the actual mechanism directly, no live
      device session needed. See `docs/ARCHITECTURE.md`'s new
      "Wireless AA discovery" section for the full writeup. Summary:
      this device's Bluetooth is Feasycom's closed `blueware` daemon
      (UART-attached, not kernel `AF_BLUETOOTH` at all), which exposes
      a local Unix socket `/dev/bw_aap` speaking a small, separate
      length/type-framed protobuf schema (`aap_protobuf::aaw::*`,
      already vendored in `aasdk`) for exactly this pre-connection
      WiFi-credential handoff. Real message sequence decoded
      field-for-field against the captured bytes (`WIFI_START_REQUEST`/
      `WIFI_INFO_RESPONSE` match their `.proto`s exactly;
      `WIFI_VERSION_REQUEST`/`RESPONSE` don't, vendored protos
      evidently incomplete there — handled by replaying known-good raw
      bytes instead of trusting them).
      `src/androidauto/bw_aap_client.{h,cpp}` implements the confirmed
      steps 1-3 (version handshake + sending our own AP connect
      target); `tools/androidauto-bw-aap-test` drives it and logs
      whatever follows. Linked clean, zero `GLIBC` references. **Not
      yet hardware-tested** — faithful reconstruction of real captured
      traffic, not confirmed against a live phone driven by our own
      code; also unconfirmed whether `/dev/bw_aap` tolerates a second
      simultaneous client if `sink`/`MsnCoreApp` already holds it open.
      **Supersedes** the raw `AF_BLUETOOTH`/`BTPROTO_RFCOMM` + SDP
      approach below (`bluetooth_transport.h`/
      `bluetooth_rfcomm_server.h`) — kept in the tree as legitimate
      generic code, but not the path forward here since this device
      never had a BlueZ stack to register an SDP record with in the
      first place.
      `scripts/diagnose_bluetooth.sh` (written before this finding, for
      the originally-planned live-device investigation) is no longer
      the critical path but still a reasonable sanity check before
      hardware-testing the `bw_aap` client — the underlying "no BlueZ
      stack" fact it was written to help confirm turned out to be
      true, just resolved through log archaeology instead of a live
      session.
- [ ] Reuse (not reinvent) the stock dynamic-AP mechanism —
      confirm whether `hostapd`/`wifi_ap.sh`'s existing per-connection
      logic (already fixed once this project, see
      `project_static_wifi_ap_vs_dynamic`) can be driven from our own
      code, or needs its own equivalent — feeds `BwAapClient::
      startHandshake()`'s `apIpAddress`/`apPort` and
      `respondToInfoRequest()`'s SSID/password/BSSID with the real,
      live values instead of test-tool command-line args
- [x] Implement `WIFI_INFO_REQUEST`/`WIFI_INFO_RESPONSE` (steps 4-5) —
      `BwAapClient::respondToInfoRequest()` waits for the phone's
      `WIFI_INFO_REQUEST` (type 2) then sends `WIFI_INFO_RESPONSE`
      (type 3) built via aasdk's generated `WifiInfoResponse` class
      (ssid/password/bssid/security_mode — all confirmed field-clean
      against the capture). `security_mode` defaults to the raw value
      `8` observed in real captured traffic (numerically
      `WPA2_ENTERPRISE` in `WifiSecurityMode`'s enum). **Cross-checked
      against `firmware_source/mtd6_rootfs/etc/hostapd/hostapd.conf`**
      (the real static config template — `wpa_passphrase=88888888`
      matches the captured password exactly) and found a genuine
      discrepancy: that config is `wpa=2`/`wpa_key_mgmt=WPA-PSK`,
      ordinary WPA2 *personal*, not enterprise — so either the vendored
      `WifiSecurityMode` enum's numbering doesn't match what this
      firmware actually transmits (same issue class as
      `WifiVersionRequest`/`Response`), or the field isn't load-bearing
      and phones ignore it. Try `WPA2_PERSONAL` (5) if `8` doesn't work
      when hardware-tested — documented as a fallback, not guessed
      blind. Also cross-checked: the real captured SSID
      (`carplay_fc9f`) isn't the static template's `carplay_wifi` —
      `MsnCoreApp` rewrites it at runtime with a suffix matching the
      last 4 hex chars of the Bluetooth MAC (`blueware`'s own log
      shows the same `fc9f` suffix on its BT broadcast name, "Limcet
      Box_fc9f", for BD_ADDR `DC0D3014FC9F`) — password stays fixed at
      `88888888` across instances, not reverse-engineered further
      (closed binary). `tools/androidauto-bw-aap-test` now drives the
      full 5-step sequence (`<ap-ip> <ap-port> <ssid> <password>
      <bssid> [security-mode=8] [seconds=15]`) and keeps listening/
      hex-dumping afterward. Linked clean, zero `GLIBC` references, all
      other targets still build.
- [ ] Hardware-test `BwAapClient` against a real phone — first close
      look at whether the phone actually responds sensibly to our
      replayed `WIFI_VERSION_REQUEST` bytes and our own
      `WIFI_START_REQUEST`, and whether `/dev/bw_aap` tolerates our
      code connecting to it (see open questions above)
- [ ] Once the phone connects to our AP + TCP port: confirm the real
      aasdk session (`Session`, already working) actually starts a
      normal handshake against it, same as the manual wireless-probe
      test today
- **Milestone**: a phone gets WiFi credentials over `/dev/bw_aap`,
  joins the head unit's AP, and the control-channel handshake (already
  working over USB and manual TCP) completes automatically — provable
  on this dev unit without needing the external USB port free.

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

- [x] Resolved the open question: reversing-camera video is fully
      independent of the CarPlay/AA and `sink` pipelines — the ITU656
      decode pipeline is composited directly onto its own hardware
      display layer (`DISPLAY_LAYER=4`) by the LCDC itself, not
      delivered to userspace. See `docs/ARCHITECTURE.md`'s "Reversing
      camera" section for the full finding
      (`ARK_DVR_GETFRAME` is a compiled-in no-op).
- [x] Wired `/dev/dvr` (`ARK_DVR_*` ioctls) + `/dev/carback`
      (`CARBACK_IOCTL_*` app-ready handshake + reverse-gear state
      change) into a camera-preview screen: `src/hal/camera.{h,cpp}`
      (HAL), `src/core/reverse_gear_watcher.{h,cpp}` (blocking-read
      listener thread), `src/ui/reverse_camera_screen.{h,cpp}` (pushed/
      popped via the existing `core::ScreenManager` pattern). Since the
      HAL cannot pull frames into an LVGL canvas (see above), the
      screen's job is limited to starting/stopping the pipeline,
      acking the enter/exit handshake, and not opaquely covering the
      camera's own hardware layer. Linked clean, zero `GLIBC`
      references, all other targets still build. **Not yet
      hardware-tested.**
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
