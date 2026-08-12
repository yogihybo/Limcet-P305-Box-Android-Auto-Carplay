#include "androidauto/wireless_session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include <sys/select.h>

#include <boost/asio.hpp>

#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include "androidauto/bw_aap_client.h"
#include "androidauto/session.h"
#include "core/hal_config.h"

namespace androidauto {

namespace {

// Real, working values from firmware_overlay/etc/wifi_ap.sh /
// firmware_source/mtd6_rootfs/etc/hostapd/hostapd.conf, not guessed --
// see wireless_session_manager.h's header comment. Configurable now
// (core/hal_config.h / firmware_overlay/etc/custom_ui/hal.conf)
// instead of hardcoded here; ApSecurityMode's real captured value
// (docs/logs/android auto log v{1,2,3}.txt) and SessionPort's
// UNCONFIRMED status are both still exactly as documented there, just
// moved to the shared config file's defaults.

std::string readWlan0Mac() {
    std::ifstream f("/sys/class/net/wlan0/address");
    std::string mac;
    std::getline(f, mac);
    if (mac.empty()) {
        std::fprintf(stderr, "androidauto: wireless session: /sys/class/net/wlan0/address unreadable "
                     "or empty\n");
    }
    return mac;
}

bool isApRunning() {
    bool running = std::system("pidof hostapd >/dev/null 2>&1") == 0;
    std::printf("androidauto: wireless session: hostapd running=%s\n", running ? "yes" : "no");
    return running;
}

}  // namespace

bool WirelessSessionManager::discoverPhoneIp(std::string & outIp, int timeoutSeconds) {
    std::printf("androidauto: wireless session: polling /proc/net/arp for wlan0 up to %ds...\n",
                timeoutSeconds);
    for (int elapsed = 0; elapsed < timeoutSeconds; ++elapsed) {
        std::ifstream arp("/proc/net/arp");
        std::string line;
        std::getline(arp, line);  // header
        int wlan0Entries = 0;
        while (std::getline(arp, line)) {
            std::istringstream iss(line);
            std::string ip, hwType, flags, hwAddr, mask, device;
            iss >> ip >> hwType >> flags >> hwAddr >> mask >> device;
            if (device != "wlan0") continue;
            ++wlan0Entries;
            if (ip == core::hal_config().wifi_ap_address()) continue;
            if (ip.rfind("192.168.43.", 0) != 0) continue;
            if (flags == "0x0") continue;  // incomplete entry
            outIp = ip;
            return true;
        }
        if (elapsed % 5 == 0) {
            std::printf("androidauto: wireless session: ARP poll %ds: %d wlan0 entries so far, "
                        "none usable yet\n", elapsed, wlan0Entries);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::fprintf(stderr, "androidauto: wireless session: ARP poll gave up after %ds, no usable "
                 "wlan0 entry found\n", timeoutSeconds);
    return false;
}

WirelessSessionManager::WirelessSessionManager() = default;

WirelessSessionManager::~WirelessSessionManager() {
    if (thread_.joinable()) {
        // No clean cancellation path today (io_service.run() blocks
        // for the session's lifetime, matching every other blocking-
        // background-thread HAL in this codebase) -- detach rather
        // than join so process exit isn't blocked on a possibly-still-
        // connecting session. Acceptable since this app has no
        // shutdown path today anyway (same convention as the
        // process-lifetime singletons elsewhere in src/hal, src/ui).
        thread_.detach();
    }
}

void WirelessSessionManager::start() {
    if (thread_.joinable()) {
        thread_.detach();
    }
    setStatus(WirelessSessionState::Idle, "Starting...");
    thread_ = std::thread(&WirelessSessionManager::run, this);
}

WirelessSessionState WirelessSessionManager::state() const {
    return state_.load(std::memory_order_acquire);
}

std::string WirelessSessionManager::statusMessage() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return statusMessage_;
}

void WirelessSessionManager::setStatus(WirelessSessionState s, std::string msg) {
    state_.store(s, std::memory_order_release);
    std::lock_guard<std::mutex> lock(statusMutex_);
    statusMessage_ = std::move(msg);
    std::printf("androidauto: wireless session: %s\n", statusMessage_.c_str());
}

bool WirelessSessionManager::ensureAccessPointUp() {
    if (isApRunning()) {
        std::printf("androidauto: wireless session: AP already up, skipping wifi_ap.sh\n");
        return true;
    }
    // Synchronous: wifi_ap.sh's own long-running daemons (hostapd -B,
    // udhcpd &) already background themselves internally -- this call
    // returns once the script's own setup (module load, wlan0-exists
    // poll loop, up to ~30s per its own comment) finishes.
    std::string script = core::hal_config().wifi_ap_script();
    std::printf("androidauto: wireless session: AP not running, launching %s...\n", script.c_str());
    int rc = std::system(script.c_str());
    std::printf("androidauto: wireless session: %s exited with rc=%d\n", script.c_str(), rc);
    if (rc != 0) {
        return false;
    }
    return isApRunning();
}

void WirelessSessionManager::run() {
    setStatus(WirelessSessionState::StartingAccessPoint, "Starting WiFi access point...");
    if (!ensureAccessPointUp()) {
        setStatus(WirelessSessionState::Failed, "Could not start the WiFi access point (wifi_ap.sh)");
        return;
    }
    std::printf("androidauto: wireless session: AP is up\n");

    std::string bssid = readWlan0Mac();
    if (bssid.empty()) {
        setStatus(WirelessSessionState::Failed, "Could not read wlan0's MAC address");
        return;
    }
    std::printf("androidauto: wireless session: wlan0 bssid=%s\n", bssid.c_str());

    const core::HalConfig & cfg = core::hal_config();

    setStatus(WirelessSessionState::BluetoothHandshake, "Connecting to blueware (/dev/bw_aap)...");
    BwAapClient bwAap;
    if (!bwAap.connect()) {
        setStatus(WirelessSessionState::Failed, "Could not open /dev/bw_aap");
        return;
    }

    // outIp/outPort start as "no override" (empty / the configured
    // default) -- startHandshake() only overwrites them if a real
    // WIFI_START_RESPONSE with those fields set actually arrives. See
    // its header comment (2026-08-12) for why this project went
    // connect-out -> listen/accept -> back to connect-out: a real test
    // of the listen/accept approach showed the phone's own AA app
    // reporting "connected" while this device's listener never saw an
    // incoming connection at all, proving the phone expects US to
    // connect to IT (matching the ORIGINAL "connection refused" result,
    // which specifically means the phone WAS reachable, just not
    // listening on the exact port we guessed) -- so the real fix is
    // reading WIFI_START_RESPONSE for the phone's actual port, not
    // flipping direction.
    std::string startRespIp;
    std::uint16_t startRespPort = cfg.wifi_session_port();
    std::printf("androidauto: wireless session: bw_aap connected, starting handshake "
                "(proposing %s:%u as our AP connect target)\n", cfg.wifi_ap_address().c_str(),
                cfg.wifi_session_port());
    if (!bwAap.startHandshake(cfg.wifi_ap_address(), cfg.wifi_session_port(), startRespIp,
                               startRespPort)) {
        setStatus(WirelessSessionState::Failed, "BW_AAP handshake (version request/response) failed");
        return;
    }
    std::printf("androidauto: wireless session: BW_AAP handshake (steps 1-3, + optional "
                "WIFI_START_RESPONSE) done -- resolved override ip='%s' port=%u\n",
                startRespIp.c_str(), startRespPort);

    setStatus(WirelessSessionState::WaitingForWifiJoin,
              "Waiting for phone to request WiFi credentials...");
    if (!bwAap.respondToInfoRequest(cfg.wifi_ap_ssid(), cfg.wifi_ap_password(), bssid,
                                     cfg.wifi_ap_security_mode(), 30)) {
        setStatus(WirelessSessionState::Failed,
                  "Phone never requested WiFi credentials (WIFI_INFO_REQUEST timeout)");
        return;
    }
    std::printf("androidauto: wireless session: WIFI_INFO_RESPONSE sent (ssid=%s), closing bw_aap\n",
                cfg.wifi_ap_ssid().c_str());
    bwAap.close();

    std::string phoneIp = startRespIp;
    if (phoneIp.empty()) {
        setStatus(WirelessSessionState::WaitingForWifiJoin,
                  "Credentials sent, waiting for phone to join AP...");
        if (!discoverPhoneIp(phoneIp, 30)) {
            setStatus(WirelessSessionState::Failed, "Phone never appeared on the AP (ARP timeout)");
            return;
        }
        std::printf("androidauto: wireless session: discovered phone at %s via ARP\n",
                    phoneIp.c_str());
    } else {
        std::printf("androidauto: wireless session: using phone ip %s from WIFI_START_RESPONSE "
                    "(skipping ARP discovery)\n", phoneIp.c_str());
    }

    setStatus(WirelessSessionState::Connecting,
              "Connecting to " + phoneIp + ":" + std::to_string(startRespPort) + "...");

    boost::asio::io_service ioService;
    aasdk::tcp::TCPWrapper tcpWrapper;
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService);
    auto connectEc = tcpWrapper.connect(*socket, phoneIp, startRespPort);
    if (connectEc) {
        setStatus(WirelessSessionState::Failed, "TCP connect to " + phoneIp + ":" +
                      std::to_string(startRespPort) + " failed: " + connectEc.message());
        return;
    }
    std::printf("androidauto: wireless session: TCP connected to %s:%u\n", phoneIp.c_str(),
                startRespPort);

    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, socket);
    auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));
    auto session = std::make_shared<Session>(ioService);
    session->start(std::move(transport));

    setStatus(WirelessSessionState::Connected, "Connected -- Android Auto session running");
    std::printf("androidauto: wireless session: handing off to Session, entering io_service.run()\n");

    // Blocks this thread for the session's lifetime -- deliberately not
    // pumped from the LVGL main loop, see header comment.
    ioService.run();

    setStatus(WirelessSessionState::Failed, "Session ended (io_service stopped)");
}

}  // namespace androidauto
