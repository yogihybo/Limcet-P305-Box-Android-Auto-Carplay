#include "androidauto/wireless_session_manager.h"
#include "androidauto/log_timing.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <sys/select.h>

#include <boost/asio.hpp>

#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include "androidauto/bluez_client.h"
#include "androidauto/bluez_stack.h"
#include "androidauto/bw_aap_client.h"
#include "androidauto/session.h"
#include "core/hal_config.h"

namespace androidauto {

namespace {

// 2026-08-19: this function has ~6 separate failure-return paths
// downstream of bluez_stack_start() succeeding (BlueZ agent/profile
// registration, the connection wait, the BW_AAP handshake steps, the
// WiFi TCP accept) -- manually calling bluez_stack_stop() at every one
// is exactly the kind of thing that's easy to miss on the next edit
// (already found 4 during this change). RAII instead: constructed
// right after a successful bluez_stack_start(), calls stop()
// unconditionally in its destructor so the chip is freed back for
// blueware on every exit path, not just the ones that remember to ask
// for it. bluez-bringup.sh stop is itself idempotent/safe to call even
// if nothing is actually running.
struct BluezStackGuard {
    ~BluezStackGuard() { androidauto::bluez_stack_stop(); }
};

// Real, working values from firmware_overlay/etc/wifi_ap.sh /
// firmware_source/mtd6_rootfs/etc/hostapd/hostapd.conf, not guessed --
// see wireless_session_manager.h's header comment. Configurable now
// (core/hal_config.h / firmware_overlay/etc/custom_ui/hal.conf)
// instead of hardcoded here; ApSecurityMode's real captured value
// (docs/logs/android auto log v{1,2,3}.txt) and SessionPort's
// UNCONFIRMED status are both still exactly as documented there, just
// moved to the shared config file's defaults.

// 2026-08-12: real, live-tested candidate for a long-running mystery --
// this project's wireless AA session repeatedly gets through the full
// handshake/ServiceDiscovery/channel-open sequence, then the phone
// drops the connection with no clear protocol-content cause (see
// project_aa_missing_auth_complete.md memory for the full chase).
// Raised the idea that /sys/class/net/wlan0/address -- read once,
// right after wifi_ap.sh/hostapd starts -- might not be the BSSID
// hostapd is actually broadcasting: some WiFi drivers (this device
// uses Realtek SDIO/USB combo chips, see wifi_ap.sh's modprobe list)
// report a different MAC via sysfs than what the driver/hostapd
// actually operates with once fully live in AP mode. Google's own
// WIFI_INFO_RESPONSE to the phone declares this BSSID explicitly --
// if the phone binds to that exact BSSID during its own connection
// attempt (rather than just the SSID) and it doesn't match what's
// really broadcasting, the phone could plausibly fail silently at
// some point during or after its own WiFi association, independent of
// anything in the aasdk protocol exchange itself -- consistent with
// this project's actual observed symptom (protocol activity looks
// healthy, then an unexplained drop with no protocol-level cause
// found after extensive cross-referencing against three real
// implementations and the stock binary on this hardware).
//
// Not confirmed as THE cause (would need a live comparison on real
// hardware between sysfs and hostapd's own reported BSSID, not done
// here) -- fixed defensively instead: hostapd_cli queries hostapd's
// own live control socket directly (real, present on this device --
// see /etc/hostapd/hostapd.conf's ctrl_interface), the actual
// authoritative source for what BSSID it's really using, rather than
// a driver-level sysfs snapshot that could in principle be stale or
// reflect a different address. Falls back to the old sysfs read if
// hostapd_cli's output doesn't parse (control socket not ready yet,
// binary missing, etc.) -- same "optional path, graceful degradation"
// convention as every other HAL access in this codebase, not a hard
// dependency swap.
std::string readHostapdBssid() {
    // Popen'd rather than a raw socket client for hostapd_cli's own
    // control protocol -- this call is on WirelessSessionManager's own
    // background thread (see run()), not the LVGL/asio hot path, and
    // hostapd_cli already exists on this rootfs (usr/bin/hostapd_cli)
    // specifically to talk to that socket correctly, including
    // whatever framing/retry behavior the real protocol needs -- not
    // worth reimplementing.
    FILE * pipe = popen("hostapd_cli -i wlan0 status 2>/dev/null", "r");
    if (pipe == nullptr) {
        return "";
    }
    std::string output;
    std::array<char, 256> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    pclose(pipe);

    const std::string key = "bssid[0]=";
    auto pos = output.find(key);
    if (pos == std::string::npos) {
        return "";
    }
    pos += key.size();
    auto end = output.find_first_of("\r\n", pos);
    return output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

std::string readWlan0Mac() {
    std::string bssid = readHostapdBssid();
    if (!bssid.empty()) {
        std::printf("%s androidauto: wireless session: BSSID from hostapd_cli (live control socket): %s\n", androidauto::logTimestamp().c_str(),
                     bssid.c_str());
        return bssid;
    }

    std::fprintf(stderr, "%s androidauto: wireless session: hostapd_cli status didn't yield a bssid -- "
                 "falling back to /sys/class/net/wlan0/address\n", androidauto::logTimestamp().c_str());
    std::ifstream f("/sys/class/net/wlan0/address");
    std::string mac;
    std::getline(f, mac);
    if (mac.empty()) {
        std::fprintf(stderr, "%s androidauto: wireless session: /sys/class/net/wlan0/address unreadable "
                     "or empty\n", androidauto::logTimestamp().c_str());
    }
    return mac;
}

bool isApRunning() {
    bool running = std::system("pidof hostapd >/dev/null 2>&1") == 0;
    std::printf("%s androidauto: wireless session: hostapd running=%s\n", androidauto::logTimestamp().c_str(), running ? "yes" : "no");
    return running;
}

}  // namespace

WirelessSessionManager::WirelessSessionManager() = default;

WirelessSessionManager::~WirelessSessionManager() {
    std::lock_guard<std::mutex> lock(threadMutex_);
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
    // See threadMutex_'s own comment (header) -- start() can race
    // against itself across two independent sidecar client connections
    // (the +AAPDEV= auto-trigger and a manual Connect tap in
    // particular), and std::thread's assignment operator calls
    // std::terminate() on a still-joinable target, so the whole
    // check-detach-reassign sequence must be atomic, not just the
    // individual std::thread calls.
    std::lock_guard<std::mutex> lock(threadMutex_);

    // See start()'s own header comment -- a session already actively
    // progressing or Connected must not be torn down by a redundant
    // second CONNECT. Deliberately checked/transitioned INSIDE the
    // same lock as the thread_ manipulation below, not before it --
    // checking state_ before acquiring threadMutex_ would leave a
    // narrow window where two concurrent start() calls both observe
    // Idle and both proceed (the run() thread doesn't transition state_
    // away from Idle until it actually starts executing, which happens
    // asynchronously after thread creation). Doing the check and the
    // setStatus() transition under the same lock that also owns
    // thread_ closes that window: by the time either caller's lock is
    // released, state_ already reflects whichever one actually won.
    WirelessSessionState current = state_.load(std::memory_order_acquire);
    if (current != WirelessSessionState::Idle && current != WirelessSessionState::Failed) {
        std::printf("%s androidauto: wireless session: start() ignored -- a session is already "
                    "active (state=%d)\n", androidauto::logTimestamp().c_str(), static_cast<int>(current));
        return;
    }

    if (thread_.joinable()) {
        thread_.detach();
    }
    setStatus(WirelessSessionState::StartingAccessPoint, "Starting...");
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
    std::printf("%s androidauto: wireless session: %s\n", androidauto::logTimestamp().c_str(), statusMessage_.c_str());
}

bool WirelessSessionManager::ensureAccessPointUp() {
    if (isApRunning()) {
        std::printf("%s androidauto: wireless session: AP already up, skipping wifi_ap.sh\n", androidauto::logTimestamp().c_str());
        return true;
    }
    // Synchronous: wifi_ap.sh's own long-running daemons (hostapd -B,
    // udhcpd &) already background themselves internally -- this call
    // returns once the script's own setup (module load, wlan0-exists
    // poll loop, up to ~30s per its own comment) finishes.
    std::string script = core::hal_config().wifi_ap_script();
    std::printf("%s androidauto: wireless session: AP not running, launching %s...\n", androidauto::logTimestamp().c_str(), script.c_str());
    int rc = std::system(script.c_str());
    std::printf("%s androidauto: wireless session: %s exited with rc=%d\n", androidauto::logTimestamp().c_str(), script.c_str(), rc);
    if (rc != 0) {
        return false;
    }
    return isApRunning();
}

void WirelessSessionManager::run() {
    // Register BlueZ profile immediately so it is available before any phone connection
    setStatus(WirelessSessionState::BluetoothHandshake, "Ensuring BlueZ stack is active...");
    if (!androidauto::bluez_stack_start()) {
        setStatus(WirelessSessionState::Failed, "Could not verify BlueZ stack");
        return;
    }

    androidauto::BluezClient bluez;
    if (!bluez.connect() || !bluez.register_agent() || !bluez.register_profile()) {
        setStatus(WirelessSessionState::Failed, "Could not register BlueZ agent/profile");
        return;
    }

    setStatus(WirelessSessionState::StartingAccessPoint, "Starting WiFi access point...");
    if (!ensureAccessPointUp()) {
        setStatus(WirelessSessionState::Failed, "Could not start the WiFi access point (wifi_ap.sh)");
        return;
    }
    std::printf("%s androidauto: wireless session: AP is up\n", androidauto::logTimestamp().c_str());

    std::string bssid = readWlan0Mac();
    if (bssid.empty()) {
        setStatus(WirelessSessionState::Failed, "Could not read wlan0's MAC address");
        return;
    }
    std::printf("%s androidauto: wireless session: wlan0 bssid=%s\n", androidauto::logTimestamp().c_str(), bssid.c_str());

    const core::HalConfig & cfg = core::hal_config();

    boost::asio::io_service ioService;
    boost::system::error_code openEc;
    boost::asio::ip::tcp::acceptor acceptor(ioService);
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), cfg.wifi_session_port());
    acceptor.open(endpoint.protocol(), openEc);
    if (!openEc) acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), openEc);
    if (!openEc) acceptor.bind(endpoint, openEc);
    if (!openEc) acceptor.listen(boost::asio::socket_base::max_connections, openEc);
    if (openEc) {
        setStatus(WirelessSessionState::Failed,
                  "Could not listen on 0.0.0.0:" + std::to_string(cfg.wifi_session_port()) + ": " +
                      openEc.message());
        return;
    }
    std::printf("%s androidauto: wireless session: WPP TCP server listening on 0.0.0.0:%u\n", androidauto::logTimestamp().c_str(),
                cfg.wifi_session_port());

    setStatus(WirelessSessionState::BluetoothHandshake, "Waiting for phone to connect over Bluetooth...");
    int rfcommFd = bluez.wait_for_connection(120);
    if (rfcommFd < 0) {
        setStatus(WirelessSessionState::Idle, "Ready for connection (Bluetooth profile listening)");
        return;
    }
    std::printf("%s androidauto: wireless session: phone connected over BlueZ RFCOMM (fd=%d)\n",
                androidauto::logTimestamp().c_str(), rfcommFd);

    BwAapClient bwAap;
    bwAap.attach(rfcommFd);

    // outIp/outPort are unused now (no ARP-based connect-out target
    // needed) but startHandshake() still reads back an optional
    // WIFI_START_RESPONSE for logging visibility -- see its own header
    // comment. cfg.wifi_ap_address() (not 0.0.0.0) is what we tell the
    // PHONE to dial, since 0.0.0.0 isn't a routable address from the
    // phone's side -- only our own bind stays on all interfaces.
    std::string startRespIp;
    std::uint16_t startRespPort = cfg.wifi_session_port();
    std::printf("%s androidauto: wireless session: bw_aap connected, starting handshake "
                "(advertising %s:%u for the phone to dial in on)\n", androidauto::logTimestamp().c_str(), cfg.wifi_ap_address().c_str(),
                cfg.wifi_session_port());
    if (!bwAap.startHandshake(cfg.wifi_ap_address(), cfg.wifi_session_port(), startRespIp,
                               startRespPort)) {
        setStatus(WirelessSessionState::Failed, "BW_AAP handshake (version request/response) failed");
        return;
    }
    std::printf("%s androidauto: wireless session: BW_AAP handshake (steps 1-3, + optional "
                "WIFI_START_RESPONSE) done\n", androidauto::logTimestamp().c_str());

    setStatus(WirelessSessionState::WaitingForWifiJoin,
              "Waiting for phone to request WiFi credentials...");
    if (!bwAap.respondToInfoRequest(cfg.wifi_ap_ssid(), cfg.wifi_ap_password(), bssid,
                                     cfg.wifi_ap_security_mode(), 30)) {
        setStatus(WirelessSessionState::Failed,
                  "Phone never requested WiFi credentials (WIFI_INFO_REQUEST timeout)");
        return;
    }
    std::printf("%s androidauto: wireless session: WIFI_INFO_RESPONSE sent (ssid=%s)\n", androidauto::logTimestamp().c_str(),
                cfg.wifi_ap_ssid().c_str());

    // 2026-08-13: waits briefly for an optional WIFI_CONNECT_STATUS
    // before closing -- see BwAapClient::waitForOptionalConnectStatus()'s
    // own header comment for the full reasoning. Real, decisive finding
    // from decompiling libAndroidAuto.so's RfcommConnectionPrivate: this
    // code used to close /dev/bw_aap immediately after sending
    // WIFI_INFO_RESPONSE, but stock's own run() loop never closes this
    // channel at all -- it keeps reading from it for the whole
    // connection's lifetime. If the phone expects to report WiFi
    // connection status back over this same still-open Bluetooth link
    // (a message this project had never once looked for before), the
    // premature close() here could plausibly be part of why sessions
    // that otherwise look healthy eventually go quiet. This is a
    // smaller, safer step than fully replicating "never close" (see
    // that function's own comment on why) -- a bounded wait, not an
    // indefinite one.
    // 15s, not 5 -- real captured hardware log (docs/logs/
    // start_msn_stock_260721.txt) shows a genuine DHCP offer/ack
    // exchange happens between WIFI_START_RESPONSE and
    // WIFI_CONNECT_STATUS, so 5s was too tight a window for what this
    // call now actually waits through (see its own updated comment).
    bwAap.waitForOptionalConnectStatus(15);
    // 2026-08-19: per docs/SESSION_KEEPALIVE_AND_TIMEOUT_HANDOFF.md,
    // building directly on this function's own comment above (already
    // decompile-confirmed stock never closes /dev/bw_aap for the whole
    // connection's lifetime, but this project only took the smaller,
    // bounded-wait step at the time) -- bwAap.close() moved from here to
    // after the AA session actually ends, below. Real hardware showed
    // sessions dying ~20-30s after connecting with a bare TCP EOF
    // (control channel error 33/TCP_TRANSFER, no AA-protocol bye-bye),
    // consistent with the phone treating a dropped Bluetooth RFCOMM
    // tether as an out-of-band "is the vehicle still there" signal and
    // tearing down the WiFi session on its own timeout. Keeping this
    // link open for the session's real duration, not just through the
    // WiFi handshake, is the fuller fix that comment already flagged as
    // the more faithful (if riskier) match to stock -- now taken.

    setStatus(WirelessSessionState::Connecting,
              "Waiting for phone to dial in on 0.0.0.0:" + std::to_string(cfg.wifi_session_port()) +
                  "...");

    // select()-based accept timeout -- accept() itself has no timeout
    // parameter, same pattern as hal/bluetooth.cpp send_command() /
    // BwAapClient::receiveFrame().
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

    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService);
    boost::system::error_code acceptEc;
    acceptor.accept(*socket, acceptEc);
    if (acceptEc) {
        setStatus(WirelessSessionState::Failed, "accept() failed: " + acceptEc.message());
        return;
    }

    // 2026-08-19: per docs/SESSION_KEEPALIVE_AND_TIMEOUT_HANDOFF.md --
    // this socket previously had no options set at all. Without
    // TCP_NODELAY, Nagle's algorithm can buffer small outgoing control
    // frames (PingResponse, InputReport, MediaAckIndication) waiting for
    // a full MSS or a delayed ACK, adding real latency to exactly the
    // traffic pingConfig's own timeout is racing against. Without
    // SO_KEEPALIVE, the kernel never probes a silently-dead WiFi link on
    // its own -- a half-open connection (AP still up, phone gone) would
    // sit idle until the next application-level ping actually needed the
    // socket, rather than being detected proactively. Best-effort: a
    // failure to set either option isn't fatal, just logged.
    boost::system::error_code nodelayEc;
    socket->set_option(boost::asio::ip::tcp::no_delay(true), nodelayEc);
    if (nodelayEc) {
        std::printf("%s androidauto: wireless session: TCP_NODELAY set failed: %s\n",
                    androidauto::logTimestamp().c_str(), nodelayEc.message().c_str());
    }
    boost::system::error_code keepaliveEc;
    socket->set_option(boost::asio::socket_base::keep_alive(true), keepaliveEc);
    if (keepaliveEc) {
        std::printf("%s androidauto: wireless session: SO_KEEPALIVE set failed: %s\n",
                    androidauto::logTimestamp().c_str(), keepaliveEc.message().c_str());
    }

    boost::system::error_code peerEc;
    auto remote = socket->remote_endpoint(peerEc);
    if (!peerEc) {
        std::printf("%s androidauto: wireless session: phone connected from %s:%u\n", androidauto::logTimestamp().c_str(),
                    remote.address().to_string().c_str(), remote.port());
    } else {
        std::printf("%s androidauto: wireless session: phone connected (remote_endpoint() "
                    "unavailable: %s)\n", androidauto::logTimestamp().c_str(), peerEc.message().c_str());
    }

    aasdk::tcp::TCPWrapper tcpWrapper;
    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, socket);
    auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));
    auto session = std::make_shared<Session>(ioService);
    session->start(std::move(transport));

    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        currentSession_ = session;
    }

    setStatus(WirelessSessionState::Connected, "Connected -- Android Auto session running");
    std::printf("%s androidauto: wireless session: handing off to Session, entering io_service.run()\n", androidauto::logTimestamp().c_str());

    // Blocks this thread for the session's lifetime -- deliberately not
    // pumped from the LVGL main loop, see header comment.
    ioService.run();

    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        currentSession_.reset();
    }

    // See this function's own comment where bwAap.waitForOptionalConnectStatus()
    // is called, above -- kept open for the AA session's real duration
    // now, closed here once it's actually over.
    std::printf("%s androidauto: wireless session: closing bw_aap\n", androidauto::logTimestamp().c_str());
    bwAap.close();
    // bluezStackGuard (still in scope, this whole function body) frees
    // the chip back for blueware once it destructs at function exit.

    setStatus(WirelessSessionState::Failed, "Session ended (io_service stopped)");
}

void WirelessSessionManager::sendInputKey(std::uint32_t keycode) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (currentSession_) {
        currentSession_->sendInputKey(keycode);
    }
}

void WirelessSessionManager::sendInputTouch(std::uint32_t x, std::uint32_t y,
                                             aap_protobuf::service::inputsource::message::PointerAction action) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (currentSession_) {
        currentSession_->sendInputTouch(x, y, action);
    }
}

void WirelessSessionManager::resumeVideoFocus() {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (currentSession_) {
        currentSession_->resumeVideoFocus();
    }
}

}  // namespace androidauto
