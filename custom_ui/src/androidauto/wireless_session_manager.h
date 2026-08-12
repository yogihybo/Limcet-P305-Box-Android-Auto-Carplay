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
//  - udhcpd's lease grant on wlan0 (started by wifi_ap.sh) -- when
//    WIFI_START_RESPONSE doesn't override the phone's IP (see below),
//    falls back to discovering it by polling for a new ARP entry on
//    the 192.168.43.0/24 subnet (excluding our own AP address), via
//    /proc/net/arp.
//  - aasdk::transport::TCPTransport / aasdk::tcp::ITCPWrapper -- only
//    implements the TCP *client* side (connect/asyncConnect, see
//    include/aasdk/TCP/TCPWrapper.hpp), so this class connects OUT to
//    the phone.
//
//  2026-08-12 TWO REVISIONS IN ONE DAY, see project memory for the
//  full story -- short version: this started as connect-out, got
//  "connection refused" on real hardware, was flipped to listen/accept
//  on the theory the head-unit-as-server aasdk reference docs +
//  WIFI_START_REQUEST's own "here's our address" framing implied
//  (see git history for that version's reasoning) -- but a SECOND real
//  hardware test of the listen/accept version showed the phone's own
//  AA app reporting "connected" while this device's listener never saw
//  an incoming connection AT ALL. That's decisive: the phone expects
//  US to connect to IT. Re-reading the FIRST test's exact symptom
//  confirms this was knowable from the start -- "connection refused"
//  (ECONNREFUSED) specifically means the TCP SYN reached a live host at
//  that IP and got an RST back because nothing was listening on that
//  PORT; it does not mean the host was unreachable or the direction was
//  wrong. So this class is back to connect-out, and the real fix for
//  the original bug is: BwAapClient::startHandshake() now also waits
//  (bounded) for an optional WIFI_START_RESPONSE (MessageId type 7)
//  after sending WIFI_START_REQUEST -- its proto has optional
//  ip_address/port fields that can carry the phone's own authoritative
//  connect-back target, overriding the guessed default port (5277) and
//  skipping ARP discovery entirely if an ip_address is also provided.
//
// STILL UNCONFIRMED, flagged honestly rather than guessed at:
//  - Whether the phone's real hardware actually SENDS a
//    WIFI_START_RESPONSE at all -- this project's own captured
//    reference traffic (docs/logs) only exercises steps 1-5, not a
//    type-7 reply, so this is genuinely unverified either way. If it
//    never arrives, this class falls back to the original ARP-
//    discovery + cfg.wifi_session_port() (5277) behavior, which is
//    exactly what got "connection refused" -- meaning if that's what
//    happens, the actual fix still isn't found and the phone's real
//    listening port remains unknown.
//  - security_mode passed to WIFI_INFO_RESPONSE (currently 8,
//    WPA2_ENTERPRISE per the vendored enum, matching real captured
//    traffic) doesn't match this AP's actual config (plain WPA2-
//    Personal, per hostapd.conf's own wpa_key_mgmt=WPA-PSK) -- a real,
//    already-documented discrepancy (see bw_aap_client.h). Try 5
//    (WPA2_PERSONAL) if 8 doesn't work.
//
// NOT YET hardware-tested end to end with the WIFI_START_RESPONSE fix
// above.
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
    bool discoverPhoneIp(std::string & outIp, int timeoutSeconds);

    std::thread thread_;
    std::atomic<WirelessSessionState> state_{WirelessSessionState::Idle};

    mutable std::mutex statusMutex_;
    std::string statusMessage_;
};

}  // namespace androidauto
