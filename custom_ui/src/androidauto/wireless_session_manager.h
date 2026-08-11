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
//  - udhcpd's lease grant on wlan0 (started by wifi_ap.sh) --
//    discovering the phone's IP once it joins by polling for a new
//    ARP entry on the 192.168.43.0/24 subnet (excluding our own AP
//    address), via /proc/net/arp. Simpler and more portable across
//    busybox builds than parsing udhcpd's binary lease-file format.
//  - aasdk::transport::TCPTransport -- confirmed (by reading
//    aasdk::tcp::ITCPWrapper's actual interface) to only implement the
//    TCP *client* side, matching real Android Auto Wireless
//    architecture: the head unit connects OUT to the phone, not the
//    reverse. Same Session class the wired (USBTransport) path uses,
//    see wireless_probe.cpp for the reference pattern this follows.
//
// STILL UNCONFIRMED, flagged honestly rather than guessed at:
//  - The TCP port the phone listens on once connected -- defaults to
//    5277, the same "commonly-cited guess... not independently
//    confirmed" value wireless_probe.h already uses. If a real phone
//    doesn't accept a connection on this port, that's the first thing
//    to check.
//  - security_mode passed to WIFI_INFO_RESPONSE (currently 8,
//    WPA2_ENTERPRISE per the vendored enum, matching real captured
//    traffic) doesn't match this AP's actual config (plain WPA2-
//    Personal, per hostapd.conf's own wpa_key_mgmt=WPA-PSK) -- a real,
//    already-documented discrepancy (see bw_aap_client.h). Try 5
//    (WPA2_PERSONAL) if 8 doesn't work.
//
// NOT YET hardware-tested end to end.
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
