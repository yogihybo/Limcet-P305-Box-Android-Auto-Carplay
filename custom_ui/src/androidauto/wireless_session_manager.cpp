#include "androidauto/wireless_session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
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
    int rc = std::system(core::hal_config().wifi_ap_script().c_str());
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

    // Start listening BEFORE the BW_AAP handshake even begins, so the
    // listen socket is already up by the time WIFI_START_REQUEST tells
    // the phone where to connect -- avoids a race where the phone tries
    // to connect before we're ready. See this file's listenForPhone()
    // and the header comment for why this replaced an earlier ARP-
    // poll-then-connect-out approach (real hardware hit "connection
    // refused" with that approach).
    boost::asio::io_service ioService;
    std::shared_ptr<boost::asio::ip::tcp::socket> phoneSocket;
    boost::system::error_code openEc;
    auto address = boost::asio::ip::address::from_string(cfg.wifi_ap_address(), openEc);
    boost::asio::ip::tcp::acceptor acceptor(ioService);
    if (!openEc) {
        boost::asio::ip::tcp::endpoint endpoint(address, cfg.wifi_session_port());
        acceptor.open(endpoint.protocol(), openEc);
        if (!openEc) acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), openEc);
        if (!openEc) acceptor.bind(endpoint, openEc);
        if (!openEc) acceptor.listen(boost::asio::socket_base::max_connections, openEc);
    }
    if (openEc) {
        setStatus(WirelessSessionState::Failed,
                  "Could not listen on " + cfg.wifi_ap_address() + ":" +
                      std::to_string(cfg.wifi_session_port()) + ": " + openEc.message());
        return;
    }
    std::printf("androidauto: wireless session: listening on %s:%u\n", cfg.wifi_ap_address().c_str(),
                cfg.wifi_session_port());

    setStatus(WirelessSessionState::BluetoothHandshake, "Connecting to blueware (/dev/bw_aap)...");
    BwAapClient bwAap;
    if (!bwAap.connect()) {
        setStatus(WirelessSessionState::Failed, "Could not open /dev/bw_aap");
        return;
    }

    std::printf("androidauto: wireless session: bw_aap connected, starting handshake "
                "(advertising %s:%u as our AP connect target)\n", cfg.wifi_ap_address().c_str(),
                cfg.wifi_session_port());
    if (!bwAap.startHandshake(cfg.wifi_ap_address(), cfg.wifi_session_port())) {
        setStatus(WirelessSessionState::Failed, "BW_AAP handshake (version request/response) failed");
        return;
    }
    std::printf("androidauto: wireless session: BW_AAP handshake (steps 1-3) done\n");

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

    setStatus(WirelessSessionState::Connecting,
              "Waiting for phone to connect to " + cfg.wifi_ap_address() + ":" +
                  std::to_string(cfg.wifi_session_port()) + "...");

    // select()-based accept timeout -- see listenForPhone()'s comment
    // above for why (accept() itself has no timeout parameter).
    fd_set readSet;
    FD_ZERO(&readSet);
    int listenFd = acceptor.native_handle();
    FD_SET(listenFd, &readSet);
    struct timeval tv {};
    tv.tv_sec = 30;
    int ready = ::select(listenFd + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) {
        setStatus(WirelessSessionState::Failed, "No incoming AA TCP connection within 30s");
        return;
    }

    phoneSocket = std::make_shared<boost::asio::ip::tcp::socket>(ioService);
    boost::system::error_code acceptEc;
    acceptor.accept(*phoneSocket, acceptEc);
    if (acceptEc) {
        setStatus(WirelessSessionState::Failed, "accept() failed: " + acceptEc.message());
        return;
    }
    boost::system::error_code peerEc;
    auto remote = phoneSocket->remote_endpoint(peerEc);
    if (!peerEc) {
        std::printf("androidauto: wireless session: phone connected from %s:%u\n",
                    remote.address().to_string().c_str(), remote.port());
    } else {
        std::printf("androidauto: wireless session: phone connected (remote_endpoint() "
                    "unavailable: %s)\n", peerEc.message().c_str());
    }

    aasdk::tcp::TCPWrapper tcpWrapper;
    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, phoneSocket);
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
