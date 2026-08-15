#include "androidauto/bw_aap_client.h"
#include "androidauto/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>

#include <thread>
#include <chrono>

#include <aap_protobuf/aaw/WifiStartRequest.pb.h>
#include <aap_protobuf/aaw/WifiStartResponse.pb.h>
#include <aap_protobuf/aaw/WifiInfoResponse.pb.h>
#include <aap_protobuf/aaw/WifiConnectionStatus.pb.h>

namespace androidauto {

namespace {
constexpr const char *kBwAapSocketPath = "/dev/bw_aap";

// The exact WIFI_VERSION_REQUEST frame observed in
// docs/logs/android auto log v1.txt (stock `sink`, BtRfcommController)
// -- replayed verbatim rather than constructed from aasdk's
// WifiVersionRequest proto, which is declared empty (`message
// WifiVersionRequest {}`) yet the real captured payload is 9 bytes.
// The vendored proto is evidently incomplete for this message; known-
// good bytes are safer than a guess at what those 9 bytes mean.
const unsigned char kWifiVersionRequestPayload[] = {0x08, 0x01, 0x10, 0x00,
                                                     0x18, 0x00, 0x20, 0xbc, 0x28};
}  // namespace

BwAapClient::BwAapClient() = default;

BwAapClient::~BwAapClient() {
    this->close();
}

bool BwAapClient::connect() {
    // 2026-08-13: retries for a few seconds instead of one immediate
    // attempt -- same reasoning as hal::init_bluetooth()'s own
    // /dev/bw_serial open ("blueware needs a moment after spawning to
    // create" the node). /dev/bw_aap is a distinct sub-channel from
    // /dev/bw_serial (the AT-command link +AAPDEV= arrives on) -- real
    // hardware symptom this fixes: auto-starting the wireless session
    // the instant +AAPDEV= fires (main.cpp's AaAutoStartWatcher) landed
    // here with a single failed connect() and silently gave up
    // (WirelessSessionState::Failed, no user-visible retry prompt),
    // while a manual "Connect" tap moments later -- by which point
    // /dev/bw_aap had come up -- succeeded every time. Not confirmed via
    // a live strace of exactly when the node appears, but the timing
    // (fails right after a BT event, succeeds again seconds later with
    // no other change) matches this exact class of race precisely.
    constexpr int kMaxAttempts = 20;
    constexpr int kRetryDelayMs = 250;  // up to ~5s total
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            std::fprintf(stderr, "%s androidauto: bw_aap socket() failed: %s\n", androidauto::logTimestamp().c_str(), std::strerror(errno));
            return false;
        }

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, kBwAapSocketPath, sizeof(addr.sun_path) - 1);

        if (::connect(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
            std::printf("%s androidauto: connected to %s (attempt %d/%d)\n", androidauto::logTimestamp().c_str(), kBwAapSocketPath,
                        attempt + 1, kMaxAttempts);
            return true;
        }

        std::fprintf(stderr, "%s androidauto: connect(%s) failed (attempt %d/%d): %s\n", androidauto::logTimestamp().c_str(),
                     kBwAapSocketPath, attempt + 1, kMaxAttempts, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        if (attempt + 1 < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
        }
    }
    return false;
}

void BwAapClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool BwAapClient::sendFrame(std::uint16_t type, const std::string &payload) {
    std::vector<unsigned char> frame;
    std::uint16_t length = static_cast<std::uint16_t>(payload.size());
    frame.push_back(static_cast<unsigned char>(length >> 8));
    frame.push_back(static_cast<unsigned char>(length & 0xff));
    frame.push_back(static_cast<unsigned char>(type >> 8));
    frame.push_back(static_cast<unsigned char>(type & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());

    std::printf("%s androidauto: bw_aap: sendFrame type=%u length=%u:", androidauto::logTimestamp().c_str(), type, length);
    for (unsigned char byte : frame) {
        std::printf("%s  %02x", androidauto::logTimestamp().c_str(), byte);
    }
    std::printf("%s \n", androidauto::logTimestamp().c_str());

    ssize_t written = ::write(fd_, frame.data(), frame.size());
    if (written != static_cast<ssize_t>(frame.size())) {
        std::fprintf(stderr, "%s androidauto: bw_aap write failed (wrote %zd/%zu bytes): %s\n", androidauto::logTimestamp().c_str(), written,
                     frame.size(), std::strerror(errno));
        return false;
    }
    return true;
}

void BwAapClient::pushBackFrame(std::uint16_t type, std::string payload) {
    hasPendingFrame_ = true;
    pendingType_ = type;
    pendingPayload_ = std::move(payload);
    std::printf("%s androidauto: bw_aap: pushing back unconsumed type=%u frame for the next "
                "receiveFrame() call\n", androidauto::logTimestamp().c_str(), type);
}

bool BwAapClient::receiveFrame(std::uint16_t &type, std::string &payload, int timeoutSeconds) {
    if (hasPendingFrame_) {
        type = pendingType_;
        payload = std::move(pendingPayload_);
        hasPendingFrame_ = false;
        std::printf("%s androidauto: bw_aap: receiveFrame returning pending type=%u frame (no socket "
                    "read)\n", androidauto::logTimestamp().c_str(), type);
        return true;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd_, &readSet);
    struct timeval tv {};
    tv.tv_sec = timeoutSeconds;

    int ready = ::select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) {
        std::fprintf(stderr, "%s androidauto: bw_aap receiveFrame: timeout/error after %ds (select "
                     "returned %d)\n", androidauto::logTimestamp().c_str(), timeoutSeconds, ready);
        return false;
    }

    unsigned char header[4];
    ssize_t headerRead = ::read(fd_, header, sizeof(header));
    if (headerRead != static_cast<ssize_t>(sizeof(header))) {
        std::fprintf(stderr, "%s androidauto: bw_aap header read failed (got %zd/%zu bytes): %s\n", androidauto::logTimestamp().c_str(),
                     headerRead, sizeof(header), std::strerror(errno));
        return false;
    }

    std::uint16_t length = (static_cast<std::uint16_t>(header[0]) << 8) | header[1];
    type = (static_cast<std::uint16_t>(header[2]) << 8) | header[3];

    payload.resize(length);
    if (length > 0) {
        ssize_t payloadRead = ::read(fd_, &payload[0], length);
        if (payloadRead != static_cast<ssize_t>(length)) {
            std::fprintf(stderr, "%s androidauto: bw_aap payload read failed (got %zd/%u bytes): %s\n", androidauto::logTimestamp().c_str(),
                         payloadRead, length, std::strerror(errno));
            return false;
        }
    }
    std::printf("%s androidauto: bw_aap: receiveFrame type=%u length=%u\n", androidauto::logTimestamp().c_str(), type, length);
    return true;
}

bool BwAapClient::startHandshake(const std::string &apIpAddress, std::uint16_t apPort,
                                  std::string &outIp, std::uint16_t &outPort) {
    std::string versionRequestPayload(reinterpret_cast<const char *>(kWifiVersionRequestPayload),
                                       sizeof(kWifiVersionRequestPayload));
    std::printf("%s androidauto: sending WIFI_VERSION_REQUEST\n", androidauto::logTimestamp().c_str());
    if (!this->sendFrame(4, versionRequestPayload)) {
        return false;
    }

    std::uint16_t responseType = 0;
    std::string responsePayload;
    if (!this->receiveFrame(responseType, responsePayload, 10)) {
        return false;
    }
    std::printf("%s androidauto: received type=%u, %zu bytes:", androidauto::logTimestamp().c_str(), responseType, responsePayload.size());
    for (unsigned char byte : responsePayload) {
        std::printf("%s  %02x", androidauto::logTimestamp().c_str(), byte);
    }
    std::printf("%s \n", androidauto::logTimestamp().c_str());
    if (responseType != 5) {
        std::fprintf(stderr, "%s androidauto: expected WIFI_VERSION_RESPONSE (type 5), got %u\n", androidauto::logTimestamp().c_str(),
                     responseType);
        return false;
    }

    aap_protobuf::aaw::WifiStartRequest startRequest;
    startRequest.set_ip_address(apIpAddress);
    startRequest.set_port(apPort);
    std::string startRequestPayload;
    if (!startRequest.SerializeToString(&startRequestPayload)) {
        std::fprintf(stderr, "%s androidauto: failed to serialize WifiStartRequest\n", androidauto::logTimestamp().c_str());
        return false;
    }
    std::printf("%s androidauto: sending WIFI_START_REQUEST (ip=%s port=%u)\n", androidauto::logTimestamp().c_str(), apIpAddress.c_str(),
                apPort);
    if (!this->sendFrame(1, startRequestPayload)) {
        return false;
    }

    // See this function's header comment (2026-08-12) -- wait briefly
    // for an optional WIFI_START_RESPONSE (type 7). Not receiving one
    // is NOT a handshake failure, just nothing to override outIp/
    // outPort with. IMPORTANT: if what actually arrives is a DIFFERENT
    // frame (real hardware showed this happens -- the phone can send
    // WIFI_INFO_REQUEST here instead of ever sending a type-7 reply),
    // push it back via pushBackFrame() rather than discarding it, so
    // respondToInfoRequest()'s own receiveFrame() call picks it up
    // instead of timing out on a frame that already arrived and would
    // otherwise have been silently thrown away -- a real regression
    // this project hit the first time this wait was added.
    std::uint16_t startRespType = 0;
    std::string startRespPayload;
    if (!this->receiveFrame(startRespType, startRespPayload, 5)) {
        std::printf("%s androidauto: no WIFI_START_RESPONSE within 5s (may be normal -- not all "
                    "captures have shown one)\n", androidauto::logTimestamp().c_str());
        return true;
    }
    if (startRespType != 7) {
        std::printf("%s androidauto: expected WIFI_START_RESPONSE (type 7) but got type=%u instead "
                    "-- pushing it back for whoever reads next\n", androidauto::logTimestamp().c_str(), startRespType);
        this->pushBackFrame(startRespType, std::move(startRespPayload));
        return true;
    }

    aap_protobuf::aaw::WifiStartResponse startResponse;
    if (!startResponse.ParseFromString(startRespPayload)) {
        std::fprintf(stderr, "%s androidauto: failed to parse WifiStartResponse\n", androidauto::logTimestamp().c_str());
        return true;
    }
    std::printf("%s androidauto: got WIFI_START_RESPONSE (status=%d, ip_address=%s, port=%u)\n", androidauto::logTimestamp().c_str(),
                static_cast<int>(startResponse.status()),
                startResponse.has_ip_address() ? startResponse.ip_address().c_str() : "(unset)",
                startResponse.has_port() ? startResponse.port() : 0);
    if (startResponse.has_ip_address() && !startResponse.ip_address().empty()) {
        outIp = startResponse.ip_address();
    }
    if (startResponse.has_port() && startResponse.port() != 0) {
        outPort = static_cast<std::uint16_t>(startResponse.port());
    }
    return true;
}

bool BwAapClient::respondToInfoRequest(const std::string &ssid, const std::string &password,
                                        const std::string &bssid, int securityMode,
                                        int timeoutSeconds) {
    std::uint16_t requestType = 0;
    std::string requestPayload;
    std::printf("%s androidauto: waiting for WIFI_INFO_REQUEST...\n", androidauto::logTimestamp().c_str());
    if (!this->receiveFrame(requestType, requestPayload, timeoutSeconds)) {
        return false;
    }
    if (requestType != 2) {
        std::fprintf(stderr, "%s androidauto: expected WIFI_INFO_REQUEST (type 2), got %u\n", androidauto::logTimestamp().c_str(), requestType);
        return false;
    }
    std::printf("%s androidauto: got WIFI_INFO_REQUEST, sending WIFI_INFO_RESPONSE "
                "(ssid=%s bssid=%s security_mode=%d)\n", androidauto::logTimestamp().c_str(),
                ssid.c_str(), bssid.c_str(), securityMode);

    aap_protobuf::aaw::WifiInfoResponse infoResponse;
    infoResponse.set_ssid(ssid);
    infoResponse.set_password(password);
    infoResponse.set_bssid(bssid);
    infoResponse.set_security_mode(
        static_cast<aap_protobuf::service::wifiprojection::message::WifiSecurityMode>(securityMode));

    std::string infoResponsePayload;
    if (!infoResponse.SerializeToString(&infoResponsePayload)) {
        std::fprintf(stderr, "%s androidauto: failed to serialize WifiInfoResponse\n", androidauto::logTimestamp().c_str());
        return false;
    }
    return this->sendFrame(3, infoResponsePayload);
}

void BwAapClient::waitForOptionalConnectStatus(int timeoutSeconds) {
    // 2026-08-13 REVISED: real captured hardware log
    // (docs/logs/start_msn_stock_260721.txt, a genuine successful stock
    // connection to the same phone this whole project tests against)
    // showed the real message order is WIFI_INFO_RESPONSE(3) ->
    // WIFI_START_RESPONSE(7) [not caught by startHandshake()'s own
    // wait, which -- also per that log -- gets the phone's
    // WIFI_INFO_REQUEST(2) instead at that point in the sequence, a
    // known/already-handled quirk] -> [real DHCP offer/ack, taking a
    // few real seconds] -> WIFI_CONNECT_STATUS(6), payload `08 00` =
    // status field = 0 = STATUS_SUCCESS, arriving only once the phone
    // has actually joined the AP and gotten a DHCP lease -- confirmed
    // from that log's own udhcpd OFFER/ACK lines sitting directly
    // between the type-7 and type-6 reads. A single receiveFrame() call
    // would either consume the leftover, never-otherwise-read type-7
    // frame (this class currently has no path that drains it after
    // startHandshake() moves on) or time out well before DHCP finishes
    // -- so this loops, logging and discarding anything that isn't type
    // 6, until it arrives or the overall deadline passes.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    std::printf("%s androidauto: waiting up to %ds for WIFI_CONNECT_STATUS (draining any "
                "interleaved frames, e.g. a late WIFI_START_RESPONSE, along the way)...\n", androidauto::logTimestamp().c_str(),
                timeoutSeconds);

    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (remaining <= 0) {
            std::printf("%s androidauto: no WIFI_CONNECT_STATUS within %ds (may be normal -- not "
                        "confirmed the phone always sends one)\n", androidauto::logTimestamp().c_str(), timeoutSeconds);
            return;
        }

        std::uint16_t type = 0;
        std::string payload;
        if (!this->receiveFrame(type, payload, static_cast<int>(remaining))) {
            std::printf("%s androidauto: no WIFI_CONNECT_STATUS within %ds (may be normal -- not "
                        "confirmed the phone always sends one)\n", androidauto::logTimestamp().c_str(), timeoutSeconds);
            return;
        }

        if (type != 6) {
            std::printf("%s androidauto: got type=%u while waiting for WIFI_CONNECT_STATUS -- "
                        "discarding, continuing to wait\n", androidauto::logTimestamp().c_str(), type);
            continue;
        }

        aap_protobuf::aaw::WifiConnectionStatus status;
        if (!status.ParseFromString(payload)) {
            std::fprintf(stderr, "%s androidauto: failed to parse WifiConnectionStatus\n", androidauto::logTimestamp().c_str());
            return;
        }
        std::printf("%s androidauto: got WIFI_CONNECT_STATUS: status=%d%s%s\n", androidauto::logTimestamp().c_str(),
                    static_cast<int>(status.status()),
                    status.has_error_message() ? " error_message=" : "",
                    status.has_error_message() ? status.error_message().c_str() : "");
        return;
    }
}

}  // namespace androidauto
