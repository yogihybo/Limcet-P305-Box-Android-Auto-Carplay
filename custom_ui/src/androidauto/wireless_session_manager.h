// Orchestrates the full wireless Android Auto connection sequence on
// its own background thread, GIVEN an already-connected Bluetooth
// RFCOMM socket fd: bring up this app's own WiFi AP, bind a TCP
// listener, run the confirmed Bluetooth-relayed credential handoff
// (WifiSetupClient) over that fd, then accept the phone's incoming
// connection and start the real aasdk Session -- see
// docs/IMPLEMENTATION_PLAN.md Phase 2's "Wireless AA" section for why
// this whole path (not wired AOAP) is the required one on this
// hardware (single external USB port, normally occupied by the boot
// rootfs drive).
//
// 2026-08-20: this class no longer knows anything about BlueZ/D-Bus at
// all -- that ownership moved to custom_ui's own hal/bluetooth.cpp
// (hal::BluezAaProfile, aa_profile_server_loop()), alongside every
// other piece of Bluetooth connectivity this project has (adapter
// power/pairing/A2DP/CoD), instead of split across two processes.
// custom_ui registers the AA RFCOMM profile, waits for a phone to
// dial in, and hands the resulting connected fd to this process (a
// SEPARATE process, still isolated from libdbus/aasdk-heavy
// dependency mixing -- see docs/ARCHITECTURE.md) over their existing
// local Unix-domain socket via SCM_RIGHTS (see
// sidecars/androidauto/main.cpp's "CONNECT_FD" command and
// hal/androidauto_client.h's sendConnectFd()). This class's own job
// starts from there: WiFi AP bring-up + the WiFi-setup handshake + the
// real aasdk session, nothing Bluetooth-specific beyond using the fd
// it's handed.
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
//    bring-up script (SSID "custom_ui_wifi" -- deliberately distinct
//    from MsnCoreApp's own real "carplay_wifi", see that script's own
//    2026-08-16 comment for why -- password "88888888", AP address
//    192.168.43.1/24, hostapd+udhcpd). Disabled in this
//    project's own rcS only because it would conflict with stock
//    sink's OWN dynamic per-connection AP -- that conflict doesn't
//    apply here since sink/MsnCoreApp and custom_ui are never run
//    concurrently (the same handoff model already used for /dev/fb0
//    and /dev/ttyHS0). Safe to invoke directly.
//  - WifiSetupClient -- the confirmed real 5-step Bluetooth-relayed WiFi
//    credential handoff (see wifi_setup_client.h).
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
// The listener binds BEFORE the WiFi-setup handshake even starts (same
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
//    already-documented discrepancy (see wifi_setup_client.h). Try 5
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

    // Starts the background thread if not already running/finished,
    // given an already-connected Bluetooth RFCOMM socket fd (from
    // custom_ui's own hal::BluezAaProfile -- see this class's own
    // header comment above). Takes ownership of rfcommFd -- this class
    // closes it once the session ends (or immediately, if start() is a
    // no-op because a session is already active; see below).
    // Non-blocking -- returns immediately, progress is reported via
    // state()/statusMessage(). Safe to call repeatedly (e.g. a "Retry"
    // button, or another phone dialing in); a previous failed/finished
    // attempt's thread is joined and a fresh one started.
    //
    // 2026-08-13: a no-op (logs, closes rfcommFd, and returns) if a
    // session is already actively progressing or Connected -- start()
    // used to unconditionally detach whatever thread_ held and launch a
    // brand new run(), even over an already-successful session. That
    // was a real, previously theoretical concern -- with the +AAPDEV=
    // auto-trigger now reliable (see main.cpp's AaAutoStartWatcher and
    // this project's own memory notes on the boot-order/debounce
    // fixes), a second CONNECT_FD can genuinely arrive while a first
    // attempt is already Connected (a stray re-detection outside the
    // debounce window, a manual Connect tap out of habit, another
    // phone, etc.) -- restarting from scratch in that case would tear
    // down/orphan a live working session for no reason. Still
    // safe/expected to call while Idle/Failed (the actual documented
    // "Retry" use case).
    void start(int rfcommFd);

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

    // Forwards one real touch sample into the current session's
    // InputChannel, if a session exists (same no-op-if-none contract as
    // sendInputKey()). Called from sidecars/androidauto/main.cpp's own
    // "TOUCH <x> <y> <action>" command handler, forwarded in turn from
    // hal/touch.cpp via hal/androidauto_client.h -- see that header's
    // own comment for the full cross-process path (the touch panel
    // itself is read by custom_ui's own process, not this one).
    void sendInputTouch(std::uint32_t x, std::uint32_t y,
                         aap_protobuf::service::inputsource::message::PointerAction action);

    // Asks the phone to resume PROJECTED video focus, if a session
    // exists (same no-op-if-none contract as sendInputKey()). Called
    // from sidecars/androidauto/main.cpp's own "RESUME" command
    // handler, in turn from hal/androidauto_client.h's
    // requestResumeVideo() -- see Session::resumeVideoFocus()'s own
    // comment for why this exists at all.
    void resumeVideoFocus();

    // 2026-08-21: forwards the MCU-headlight-driven night-mode state
    // into the current session's SensorChannel, if a session exists
    // (same no-op-if-none contract as sendInputKey()/sendInputTouch()).
    // Called from sidecars/androidauto/main.cpp's own "NIGHT <0|1>"
    // command handler, in turn from hal/androidauto_client.h's
    // sendNightMode() -- see SensorChannel::setNightMode()'s own
    // comment for what actually happens on the wire.
    void sendNightMode(bool nightMode);

private:
    void run(int rfcommFd);
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
    // 2026-08-20: real, non-empty default -- statusMessage_ used to
    // only ever get set inside setStatus(), first called from run(),
    // which used to fire immediately at sidecar boot (the old self-
    // driven BlueZ flow). Now that this class only starts once
    // custom_ui hands it an already-connected fd (see this class's own
    // header comment), run() may not fire for minutes, or ever, in a
    // given boot -- leaving this string empty that whole time. Real
    // hardware symptom this caused: ui/android_auto_screen.cpp's status
    // row (state_body + detail_body, under the Connect button) parses
    // statusLine()'s "STATE Idle <this string>" reply -- an empty
    // string here isn't a missing/removed widget, just blank text next
    // to a real (non-blank) "Idle" label, easy to mistake for the whole
    // status line having vanished.
    std::string statusMessage_ = "Waiting for phone to connect over Bluetooth...";

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
