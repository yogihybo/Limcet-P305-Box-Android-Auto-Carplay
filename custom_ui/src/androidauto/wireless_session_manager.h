// Orchestrates the full wireless Android Auto connection sequence on
// its own background thread: bring up this app's own WiFi AP, bind a
// TCP listener, run the confirmed Bluetooth-relayed credential handoff
// (BwAapClient), then accept the phone's incoming connection and start
// the real aasdk Session -- see docs/IMPLEMENTATION_PLAN.md Phase 2's
// "Wireless AA" section for why this whole path (not wired AOAP) is
// the required one on this hardware (single external USB port,
// normally occupied by the boot rootfs drive).
//
// Runs entirely on its own thread with its own boost::asio::io_service
// -- deliberately NOT sharing/pumping the LVGL main loop's thread, same
// "don't touch LVGL/ScreenManager from any other thread" rule
// core::ReverseGearWatcher documents (see reverse_gear_watcher.h).
// UI code polls state()/statusMessage() from the main loop instead of
// this class touching anything LVGL-related directly.
//
// Real, confirmed pieces this stitches together:
//  - firmware_overlay/etc/wifi_ap.sh -- a real, already-working AP
//    bring-up script (SSID "carplay_wifi", password "88888888", AP
//    address 192.168.43.1/24, hostapd+udhcpd). Disabled in this
//    project's own rcS only because it would conflict with stock
//    sink's OWN dynamic per-connection AP -- that conflict doesn't
//    apply here since sink/MsnCoreApp and custom_ui are never run
//    concurrently (the same handoff model already used for /dev/fb0
//    and /dev/ttyHS0). Safe to invoke directly.
//  - BwAapClient -- the confirmed real 5-step Bluetooth-relayed WiFi
//    credential handoff (see bw_aap_client.h).
//
// 2026-08-12, THREE REVISIONS IN ONE DAY -- see project memory
// (project_aa_wireless_tcp_direction_fix) for the full blow-by-blow.
// Short version: connect-out (ECONNREFUSED) -> listen/accept on the
// specific AP address (phone reported "connected" but our listener
// never fired) -> connect-out again reading WIFI_START_RESPONSE
// (ECONNREFUSED again, on two different guessed ports) -> THIS
// version, informed by a real confirmed-working open-source
// implementation (github.com/mossyhub/openautolink,
// app/src/main/java/.../hotspot/WppTcpServer.kt) rather than more
// guessing:
//
//   Google's real WPP (WiFi Projection Protocol) has the HEAD UNIT as
//   the TCP server -- the phone dials in after the Bluetooth handshake
//   completes. That project's own code comment describes hitting this
//   exact project's exact symptom and names the cause outright:
//   "gearhead accepted our SDP advert, dialled our RFCOMM socket,
//   accepted WifiStartRequest with STATUS_SUCCESS, parsed our
//   WifiInfoResponse and validated the BSSID -- then went quiet,
//   because we had advertised a port with nothing bound to it."
//
// Two concrete corrections vs. this project's own earlier (reverted)
// listen/accept attempt, both taken directly from that reference:
//   1. Bind 0.0.0.0 (ALL interfaces), not the specific AP address --
//      their own comment: "we do not know which local address [the
//      phone] will use until it arrives". This is the most likely
//      reason the earlier attempt saw the phone report "connected"
//      while the listener never fired.
//   2. Default port back to 5277 (WPP's real, confirmed default) --
//      5288 was an unconfirmed guess from earlier the same day and is
//      wrong.
// The listener binds BEFORE the BW_AAP handshake even starts (same
// structural timing the earlier attempt already got right, and which
// that reference project also stresses: "AA reaches the [listener]
// within ~2s of the Bluetooth handshake").
//
// run() no longer does ARP-based phone-IP discovery at all -- with the
// head unit as the server, the phone's IP is never needed, only its
// incoming connection.
//
// STILL UNCONFIRMED / open:
//  - Not yet hardware-tested with this specific fix (0.0.0.0 bind).
//  - security_mode passed to WIFI_INFO_RESPONSE (currently 8,
//    WPA2_ENTERPRISE per the vendored enum, matching real captured
//    traffic) doesn't match this AP's actual config (plain WPA2-
//    Personal, per hostapd.conf's own wpa_key_mgmt=WPA-PSK) -- a real,
//    already-documented discrepancy (see bw_aap_client.h). Try 5
//    (WPA2_PERSONAL) if 8 doesn't work.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "androidauto/session.h"

namespace androidauto {

enum class WirelessSessionState {
    Idle,
    StartingAccessPoint,
    BluetoothHandshake,
    WaitingForWifiJoin,
    Connecting,
    Connected,
    Failed,
};

class WirelessSessionManager {
public:
    WirelessSessionManager();
    ~WirelessSessionManager();

    WirelessSessionManager(const WirelessSessionManager &) = delete;
    WirelessSessionManager & operator=(const WirelessSessionManager &) = delete;

    // Starts the background thread if not already running/finished.
    // Non-blocking -- returns immediately, progress is reported via
    // state()/statusMessage(). Safe to call repeatedly (e.g. a "Retry"
    // button); a previous failed/finished attempt's thread is joined
    // and a fresh one started.
    //
    // 2026-08-13: now a no-op (logs and returns) if a session is
    // already actively progressing or Connected -- start() used to
    // unconditionally detach whatever thread_ held and launch a brand
    // new run(), even over an already-successful session. That was a
    // real, previously theoretical concern -- with the +AAPDEV=
    // auto-trigger now reliable (see main.cpp's AaAutoStartWatcher and
    // this project's own memory notes on the boot-order/debounce
    // fixes), a second CONNECT can genuinely arrive while a first
    // attempt is already Connected (a stray re-detection outside the
    // debounce window, a manual Connect tap out of habit, etc.) --
    // restarting from scratch in that case would tear down/orphan a
    // live working session for no reason. Still safe/expected to call
    // while Idle/Failed (the actual documented "Retry" use case).
    void start();

    WirelessSessionState state() const;

    // Human-readable detail for the UI (e.g. "Waiting for phone to
    // join AP...", or the reason a Failed state happened). Not
    // lock-free (plain std::string) -- low-frequency access (UI
    // polling a few times/sec), not on any hot path.
    std::string statusMessage() const;

    // Forwards a real Android KeyEvent keycode into the current
    // session's InputChannel, if a session actually exists right now
    // (see Session::sendInputKey()'s own comment for the exact
    // keycodes and why) -- no-op, not an error, if there's no live
    // session (e.g. the physical knob was turned before any AA
    // connection exists at all). Called from sidecars/androidauto/
    // main.cpp's own "KEY <code>" command handler, forwarded in turn
    // from hal/knob.cpp via hal/androidauto_client.h -- see that
    // header's own comment for the full cross-process path (the knob
    // itself is read by custom_ui's own process, not this one).
    void sendInputKey(std::uint32_t keycode);

private:
    void run();
    void setStatus(WirelessSessionState s, std::string msg);
    bool ensureAccessPointUp();

    // 2026-08-13: guards thread_ itself (joinable()-check/detach/
    // reassign in start(), detach() in the destructor) -- start() is
    // called from sidecars/androidauto/main.cpp's handle_connection(),
    // one thread per accepted socket connection, and both
    // main.cpp's AaAutoStartWatcher (the +AAPDEV= auto-trigger) and
    // ui/android_auto_screen.cpp's manual Connect button each open
    // their own independent connection to the sidecar -- so two
    // concurrent CONNECT commands (auto-trigger firing right as a user
    // taps Connect, or a double-tap) previously raced unsynchronized on
    // this member. std::thread's assignment operator calls
    // std::terminate() if the target is still joinable, so the visible
    // symptom of that race would be the whole sidecar process abruptly
    // dying at the exact moment two start() calls overlapped.
    std::mutex threadMutex_;
    std::thread thread_;
    std::atomic<WirelessSessionState> state_{WirelessSessionState::Idle};

    mutable std::mutex statusMutex_;
    std::string statusMessage_;

    // Set once in run() right after the Session is constructed,
    // cleared when run() returns (session ended, one way or another)
    // -- lets sendInputKey() (called from a completely different
    // thread, the socket connection handling the "KEY <code>" command)
    // safely reach the live session without touching its own
    // io_service/strand from outside; Session::sendInputKey() ->
    // InputChannel::sendKey() both do their own real work (protobuf
    // send) synchronously off this call, same as every other
    // fire-and-forget send in this codebase -- not routed through the
    // session's own strand, since InputChannel's sendInputReport() is
    // itself thread-safe-enough for this (aasdk's SendPromise/channel
    // machinery handles its own internal sequencing).
    mutable std::mutex sessionMutex_;
    Session::Pointer currentSession_;
};

}  // namespace androidauto
