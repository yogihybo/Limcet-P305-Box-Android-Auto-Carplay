// Orchestrates the full wireless Android Auto connection sequence on
// its own background thread: bring up this app's own WiFi AP, run the
// confirmed Bluetooth-relayed credential handoff (BwAapClient), wait
// for the phone to actually join the AP, then connect out to it and
// start the real aasdk Session -- see docs/IMPLEMENTATION_PLAN.md
// Phase 2's "Wireless AA" section for why this whole path (not wired
// AOAP) is the required one on this hardware (single external USB
// port, normally occupied by the boot rootfs drive).
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
//  - 2026-08-12 REVISED, hardware-tested-and-corrected: this used to
//    poll /proc/net/arp for the phone's IP and connect OUT to it, on
//    the assumed premise that aasdk::transport::TCPTransport /
//    aasdk::tcp::ITCPWrapper only implements the TCP *client* side.
//    A real hardware run got exactly that far -- BW_AAP handshake
//    completed, phone showed the AP-join photo/prompt and accepted --
//    then failed with "TCP connect failed: connection refused". Two
//    independent pieces of evidence say the connect-out direction was
//    simply backwards, not a port/timing issue:
//      1. third_party/aasdk's OWN vendored docs (TESTING.md,
//         TROUBLESHOOTING.md, QUICK_REFERENCE.md) all show the
//         reference usage as the head unit BINDING/LISTENING
//         (`transport->bind("0.0.0.0", 5277)`, "Listening on port
//         5277" / "Client connected from ...") with the phone as the
//         connecting client.
//      2. BwAapClient::startHandshake() (see bw_aap_client.cpp) sends
//         a real WIFI_START_REQUEST containing OUR OWN AP address and
//         port, i.e. this device explicitly tells the phone "connect
//         to me at this ip:port" -- there would be no reason to send
//         our own address as a connect target if we intended to be
//         the one connecting out.
//    So this class now opens a listening TCP socket on
//    cfg.wifi_ap_address():cfg.wifi_session_port() (bound BEFORE the
//    BW_AAP handshake starts, so it's already listening the instant
//    the phone tries to connect) and blocks on accept() once the
//    BW_AAP credential handoff finishes -- no ARP polling needed at
//    all, since we don't need the phone's IP for anything anymore.
//    aasdk's TCPWrapper itself has no listen/accept support (only
//    connect/asyncConnect -- see include/aasdk/TCP/TCPWrapper.hpp), so
//    the listen+accept step uses a plain boost::asio::ip::tcp::acceptor
//    directly; the resulting connected socket is then handed to
//    aasdk::tcp::TCPEndpoint exactly as before (it doesn't care whether
//    the socket came from connect() or accept()).
//
// STILL UNCONFIRMED, flagged honestly rather than guessed at:
//  - The TCP port the phone connects to -- defaults to 5277, the same
//    "commonly-cited guess... not independently confirmed" value
//    wireless_probe.h already uses. This is the value we OURSELVES
//    send the phone via WIFI_START_REQUEST (see above), so as long as
//    we listen on the same port we advertise, the specific number
//    shouldn't matter -- but it hasn't been hardware-confirmed since
//    the direction fix above (the prior test run failed before this
//    fix existed).
//  - BwAapClient::startHandshake() sends WIFI_START_REQUEST but never
//    reads back a WIFI_START_RESPONSE (MessageId type 7), whose proto
//    has optional ip_address/port fields that could carry the phone's
//    own authoritative override of what we proposed. Not read/acted on
//    here -- flagged, not fixed, since the real capture this project's
//    docs are based on only exercises steps 1-5, not a type-7 reply.
//  - security_mode passed to WIFI_INFO_RESPONSE (currently 8,
//    WPA2_ENTERPRISE per the vendored enum, matching real captured
//    traffic) doesn't match this AP's actual config (plain WPA2-
//    Personal, per hostapd.conf's own wpa_key_mgmt=WPA-PSK) -- a real,
//    already-documented discrepancy (see bw_aap_client.h). Try 5
//    (WPA2_PERSONAL) if 8 doesn't work.
//
// NOT YET hardware-tested end to end with the listen/accept fix above.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

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
    void start();

    WirelessSessionState state() const;

    // Human-readable detail for the UI (e.g. "Waiting for phone to
    // join AP...", or the reason a Failed state happened). Not
    // lock-free (plain std::string) -- low-frequency access (UI
    // polling a few times/sec), not on any hot path.
    std::string statusMessage() const;

private:
    void run();
    void setStatus(WirelessSessionState s, std::string msg);
    bool ensureAccessPointUp();

    std::thread thread_;
    std::atomic<WirelessSessionState> state_{WirelessSessionState::Idle};

    mutable std::mutex statusMutex_;
    std::string statusMessage_;
};

}  // namespace androidauto
