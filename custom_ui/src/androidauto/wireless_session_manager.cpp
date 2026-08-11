#include "androidauto/wireless_session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include <boost/asio.hpp>

#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include "androidauto/bw_aap_client.h"
#include "androidauto/session.h"

namespace androidauto {

namespace {

// See wireless_session_manager.h's header comment: real, working
// values from firmware_overlay/etc/wifi_ap.sh /
// firmware_source/mtd6_rootfs/etc/hostapd/hostapd.conf, not guessed.
constexpr const char * kApScript = "/etc/wifi_ap.sh";
constexpr const char * kApAddress = "192.168.43.1";
constexpr const char * kApSsid = "carplay_wifi";
constexpr const char * kApPassword = "88888888";
// Real captured value (docs/logs/android auto log v{1,2,3}.txt), see
// bw_aap_client.h's own comment on the WPA2_ENTERPRISE-vs-actual-
// WPA2-Personal discrepancy this doesn't yet resolve.
constexpr int kApSecurityMode = 8;
// UNCONFIRMED -- see header comment.
constexpr std::uint16_t kSessionPort = 5277;

std::string readWlan0Mac() {
    std::ifstream f("/sys/class/net/wlan0/address");
    std::string mac;
    std::getline(f, mac);
    return mac;
}

bool isApRunning() {
    return std::system("pidof hostapd >/dev/null 2>&1") == 0;
}

}  // namespace

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
        return true;
    }
    // Synchronous: wifi_ap.sh's own long-running daemons (hostapd -B,
    // udhcpd &) already background themselves internally -- this call
    // returns once the script's own setup (module load, wlan0-exists
    // poll loop, up to ~30s per its own comment) finishes.
    int rc = std::system(kApScript);
    if (rc != 0) {
        return false;
    }
    return isApRunning();
}

bool WirelessSessionManager::discoverPhoneIp(std::string & outIp, int timeoutSeconds) {
    for (int elapsed = 0; elapsed < timeoutSeconds; ++elapsed) {
        std::ifstream arp("/proc/net/arp");
        std::string line;
        std::getline(arp, line);  // header
        while (std::getline(arp, line)) {
            std::istringstream iss(line);
            std::string ip, hwType, flags, hwAddr, mask, device;
            iss >> ip >> hwType >> flags >> hwAddr >> mask >> device;
            if (device != "wlan0") continue;
            if (ip == kApAddress) continue;
            if (ip.rfind("192.168.43.", 0) != 0) continue;
            if (flags == "0x0") continue;  // incomplete entry
            outIp = ip;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

void WirelessSessionManager::run() {
    setStatus(WirelessSessionState::StartingAccessPoint, "Starting WiFi access point...");
    if (!ensureAccessPointUp()) {
        setStatus(WirelessSessionState::Failed, "Could not start the WiFi access point (wifi_ap.sh)");
        return;
    }

    std::string bssid = readWlan0Mac();
    if (bssid.empty()) {
        setStatus(WirelessSessionState::Failed, "Could not read wlan0's MAC address");
        return;
    }

    setStatus(WirelessSessionState::BluetoothHandshake, "Connecting to blueware (/dev/bw_aap)...");
    BwAapClient bwAap;
    if (!bwAap.connect()) {
        setStatus(WirelessSessionState::Failed, "Could not open /dev/bw_aap");
        return;
    }

    if (!bwAap.startHandshake(kApAddress, kSessionPort)) {
        setStatus(WirelessSessionState::Failed, "BW_AAP handshake (version request/response) failed");
        return;
    }

    setStatus(WirelessSessionState::WaitingForWifiJoin,
              "Waiting for phone to request WiFi credentials...");
    if (!bwAap.respondToInfoRequest(kApSsid, kApPassword, bssid, kApSecurityMode, 30)) {
        setStatus(WirelessSessionState::Failed,
                  "Phone never requested WiFi credentials (WIFI_INFO_REQUEST timeout)");
        return;
    }
    bwAap.close();

    setStatus(WirelessSessionState::WaitingForWifiJoin, "Credentials sent, waiting for phone to join AP...");
    std::string phoneIp;
    if (!discoverPhoneIp(phoneIp, 30)) {
        setStatus(WirelessSessionState::Failed, "Phone never appeared on the AP (ARP timeout)");
        return;
    }

    setStatus(WirelessSessionState::Connecting,
              "Connecting to " + phoneIp + ":" + std::to_string(kSessionPort) + "...");

    boost::asio::io_service ioService;
    aasdk::tcp::TCPWrapper tcpWrapper;
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService);
    auto ec = tcpWrapper.connect(*socket, phoneIp, kSessionPort);
    if (ec) {
        setStatus(WirelessSessionState::Failed, "TCP connect failed: " + ec.message());
        return;
    }

    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, socket);
    auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));
    auto session = std::make_shared<Session>(ioService);
    session->start(std::move(transport));

    setStatus(WirelessSessionState::Connected, "Connected -- Android Auto session running");

    // Blocks this thread for the session's lifetime -- deliberately not
    // pumped from the LVGL main loop, see header comment.
    ioService.run();

    setStatus(WirelessSessionState::Failed, "Session ended (io_service stopped)");
}

}  // namespace androidauto
