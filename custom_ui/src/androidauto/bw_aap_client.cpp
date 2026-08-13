#include "androidauto/bw_aap_client.h"

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
            std::fprintf(stderr, "androidauto: bw_aap socket() failed: %s\n", std::strerror(errno));
            return false;
        }

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, kBwAapSocketPath, sizeof(addr.sun_path) - 1);

        if (::connect(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
            std::printf("androidauto: connected to %s (attempt %d/%d)\n", kBwAapSocketPath,
                        attempt + 1, kMaxAttempts);
            return true;
        }

        std::fprintf(stderr, "androidauto: connect(%s) failed (attempt %d/%d): %s\n",
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

    std::printf("androidauto: bw_aap: sendFrame type=%u length=%u:", type, length);
    for (unsigned char byte : frame) {
        std::printf(" %02x", byte);
    }
    std::printf("\n");

    ssize_t written = ::write(fd_, frame.data(), frame.size());
    if (written != static_cast<ssize_t>(frame.size())) {
        std::fprintf(stderr, "androidauto: bw_aap write failed (wrote %zd/%zu bytes): %s\n", written,
                     frame.size(), std::strerror(errno));
        return false;
    }
    return true;
}

void BwAapClient::pushBackFrame(std::uint16_t type, std::string payload) {
    hasPendingFrame_ = true;
    pendingType_ = type;
    pendingPayload_ = std::move(payload);
    std::printf("androidauto: bw_aap: pushing back unconsumed type=%u frame for the next "
                "receiveFrame() call\n", type);
}

bool BwAapClient::receiveFrame(std::uint16_t &type, std::string &payload, int timeoutSeconds) {
    if (hasPendingFrame_) {
        type = pendingType_;
        payload = std::move(pendingPayload_);
        hasPendingFrame_ = false;
        std::printf("androidauto: bw_aap: receiveFrame returning pending type=%u frame (no socket "
                    "read)\n", type);
        return true;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd_, &readSet);
    struct timeval tv {};
    tv.tv_sec = timeoutSeconds;

    int ready = ::select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) {
        std::fprintf(stderr, "androidauto: bw_aap receiveFrame: timeout/error after %ds (select "
                     "returned %d)\n", timeoutSeconds, ready);
        return false;
    }

    unsigned char header[4];
    ssize_t headerRead = ::read(fd_, header, sizeof(header));
    if (headerRead != static_cast<ssize_t>(sizeof(header))) {
        std::fprintf(stderr, "androidauto: bw_aap header read failed (got %zd/%zu bytes): %s\n",
                     headerRead, sizeof(header), std::strerror(errno));
        return false;
    }

    std::uint16_t length = (static_cast<std::uint16_t>(header[0]) << 8) | header[1];
    type = (static_cast<std::uint16_t>(header[2]) << 8) | header[3];

    payload.resize(length);
    if (length > 0) {
        ssize_t payloadRead = ::read(fd_, &payload[0], length);
        if (payloadRead != static_cast<ssize_t>(length)) {
            std::fprintf(stderr, "androidauto: bw_aap payload read failed (got %zd/%u bytes): %s\n",
                         payloadRead, length, std::strerror(errno));
            return false;
        }
    }
    std::printf("androidauto: bw_aap: receiveFrame type=%u length=%u\n", type, length);
    return true;
}

bool BwAapClient::startHandshake(const std::string &apIpAddress, std::uint16_t apPort,
                                  std::string &outIp, std::uint16_t &outPort) {
    std::string versionRequestPayload(reinterpret_cast<const char *>(kWifiVersionRequestPayload),
                                       sizeof(kWifiVersionRequestPayload));
    std::printf("androidauto: sending WIFI_VERSION_REQUEST\n");
    if (!this->sendFrame(4, versionRequestPayload)) {
        return false;
    }

    std::uint16_t responseType = 0;
    std::string responsePayload;
    if (!this->receiveFrame(responseType, responsePayload, 10)) {
        return false;
    }
    std::printf("androidauto: received type=%u, %zu bytes:", responseType, responsePayload.size());
    for (unsigned char byte : responsePayload) {
        std::printf(" %02x", byte);
    }
    std::printf("\n");
    if (responseType != 5) {
        std::fprintf(stderr, "androidauto: expected WIFI_VERSION_RESPONSE (type 5), got %u\n",
                     responseType);
        return false;
    }

    aap_protobuf::aaw::WifiStartRequest startRequest;
    startRequest.set_ip_address(apIpAddress);
    startRequest.set_port(apPort);
    std::string startRequestPayload;
    if (!startRequest.SerializeToString(&startRequestPayload)) {
        std::fprintf(stderr, "androidauto: failed to serialize WifiStartRequest\n");
        return false;
    }
    std::printf("androidauto: sending WIFI_START_REQUEST (ip=%s port=%u)\n", apIpAddress.c_str(),
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
        std::printf("androidauto: no WIFI_START_RESPONSE within 5s (may be normal -- not all "
                    "captures have shown one)\n");
        return true;
    }
    if (startRespType != 7) {
        std::printf("androidauto: expected WIFI_START_RESPONSE (type 7) but got type=%u instead "
                    "-- pushing it back for whoever reads next\n", startRespType);
        this->pushBackFrame(startRespType, std::move(startRespPayload));
        return true;
    }

    aap_protobuf::aaw::WifiStartResponse startResponse;
    if (!startResponse.ParseFromString(startRespPayload)) {
        std::fprintf(stderr, "androidauto: failed to parse WifiStartResponse\n");
        return true;
    }
    std::printf("androidauto: got WIFI_START_RESPONSE (status=%d, ip_address=%s, port=%u)\n",
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
    std::printf("androidauto: waiting for WIFI_INFO_REQUEST...\n");
    if (!this->receiveFrame(requestType, requestPayload, timeoutSeconds)) {
        return false;
    }
    if (requestType != 2) {
        std::fprintf(stderr, "androidauto: expected WIFI_INFO_REQUEST (type 2), got %u\n", requestType);
        return false;
    }
    std::printf("androidauto: got WIFI_INFO_REQUEST, sending WIFI_INFO_RESPONSE "
                "(ssid=%s bssid=%s security_mode=%d)\n",
                ssid.c_str(), bssid.c_str(), securityMode);

    aap_protobuf::aaw::WifiInfoResponse infoResponse;
    infoResponse.set_ssid(ssid);
    infoResponse.set_password(password);
    infoResponse.set_bssid(bssid);
    infoResponse.set_security_mode(
        static_cast<aap_protobuf::service::wifiprojection::message::WifiSecurityMode>(securityMode));

    std::string infoResponsePayload;
    if (!infoResponse.SerializeToString(&infoResponsePayload)) {
        std::fprintf(stderr, "androidauto: failed to serialize WifiInfoResponse\n");
        return false;
    }
    return this->sendFrame(3, infoResponsePayload);
}

void BwAapClient::waitForOptionalConnectStatus(int timeoutSeconds) {
    std::uint16_t type = 0;
    std::string payload;
    std::printf("androidauto: waiting up to %ds for an optional WIFI_CONNECT_STATUS...\n",
                timeoutSeconds);
    if (!this->receiveFrame(type, payload, timeoutSeconds)) {
        std::printf("androidauto: no WIFI_CONNECT_STATUS within %ds (may be normal -- not "
                    "confirmed the phone always sends one)\n", timeoutSeconds);
        return;
    }
    if (type != 6) {
        std::printf("androidauto: expected WIFI_CONNECT_STATUS (type 6) but got type=%u instead "
                    "-- pushing it back for whoever reads next\n", type);
        this->pushBackFrame(type, std::move(payload));
        return;
    }

    aap_protobuf::aaw::WifiConnectionStatus status;
    if (!status.ParseFromString(payload)) {
        std::fprintf(stderr, "androidauto: failed to parse WifiConnectionStatus\n");
        return;
    }
    std::printf("androidauto: got WIFI_CONNECT_STATUS: status=%d%s%s\n",
                static_cast<int>(status.status()),
                status.has_error_message() ? " error_message=" : "",
                status.has_error_message() ? status.error_message().c_str() : "");
}

}  // namespace androidauto
