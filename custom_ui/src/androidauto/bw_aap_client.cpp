#include "androidauto/bw_aap_client.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>

#include <aap_protobuf/aaw/WifiStartRequest.pb.h>
#include <aap_protobuf/aaw/WifiInfoResponse.pb.h>

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
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        std::fprintf(stderr, "androidauto: bw_aap socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kBwAapSocketPath, sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "androidauto: connect(%s) failed: %s\n", kBwAapSocketPath,
                     std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    std::printf("androidauto: connected to %s\n", kBwAapSocketPath);
    return true;
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

    ssize_t written = ::write(fd_, frame.data(), frame.size());
    if (written != static_cast<ssize_t>(frame.size())) {
        std::fprintf(stderr, "androidauto: bw_aap write failed: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

bool BwAapClient::receiveFrame(std::uint16_t &type, std::string &payload, int timeoutSeconds) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd_, &readSet);
    struct timeval tv {};
    tv.tv_sec = timeoutSeconds;

    int ready = ::select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
    if (ready <= 0) {
        std::fprintf(stderr, "androidauto: bw_aap read timeout/error\n");
        return false;
    }

    unsigned char header[4];
    ssize_t headerRead = ::read(fd_, header, sizeof(header));
    if (headerRead != static_cast<ssize_t>(sizeof(header))) {
        std::fprintf(stderr, "androidauto: bw_aap header read failed: %s\n", std::strerror(errno));
        return false;
    }

    std::uint16_t length = (static_cast<std::uint16_t>(header[0]) << 8) | header[1];
    type = (static_cast<std::uint16_t>(header[2]) << 8) | header[3];

    payload.resize(length);
    if (length > 0) {
        ssize_t payloadRead = ::read(fd_, &payload[0], length);
        if (payloadRead != static_cast<ssize_t>(length)) {
            std::fprintf(stderr, "androidauto: bw_aap payload read failed: %s\n", std::strerror(errno));
            return false;
        }
    }
    return true;
}

bool BwAapClient::startHandshake(const std::string &apIpAddress, std::uint16_t apPort) {
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
    return this->sendFrame(1, startRequestPayload);
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

}  // namespace androidauto
