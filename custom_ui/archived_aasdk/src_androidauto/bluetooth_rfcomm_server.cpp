#include "androidauto/bluetooth_rfcomm_server.h"
#include "androidauto/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

namespace androidauto {

int accept_rfcomm_connection(std::uint8_t channel) {
    int listenFd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (listenFd < 0) {
        std::fprintf(stderr, "%s androidauto: RFCOMM socket() failed: %s\n", androidauto::logTimestamp().c_str(), std::strerror(errno));
        return -1;
    }

    struct sockaddr_rc localAddr {};
    localAddr.rc_family = AF_BLUETOOTH;
    localAddr.rc_bdaddr = {{0, 0, 0, 0, 0, 0}};  // BDADDR_ANY -- listen on any local adapter
    localAddr.rc_channel = channel;

    if (::bind(listenFd, reinterpret_cast<struct sockaddr *>(&localAddr), sizeof(localAddr)) < 0) {
        std::fprintf(stderr, "%s androidauto: RFCOMM bind() to channel %u failed: %s\n", androidauto::logTimestamp().c_str(), channel,
                     std::strerror(errno));
        ::close(listenFd);
        return -1;
    }

    if (::listen(listenFd, 1) < 0) {
        std::fprintf(stderr, "%s androidauto: RFCOMM listen() failed: %s\n", androidauto::logTimestamp().c_str(), std::strerror(errno));
        ::close(listenFd);
        return -1;
    }

    std::printf("%s androidauto: RFCOMM listening on channel %u, waiting for a phone to connect...\n", androidauto::logTimestamp().c_str(), channel);

    struct sockaddr_rc remoteAddr {};
    socklen_t remoteLen = sizeof(remoteAddr);
    int connFd = ::accept(listenFd, reinterpret_cast<struct sockaddr *>(&remoteAddr), &remoteLen);
    ::close(listenFd);

    if (connFd < 0) {
        std::fprintf(stderr, "%s androidauto: RFCOMM accept() failed: %s\n", androidauto::logTimestamp().c_str(), std::strerror(errno));
        return -1;
    }

    std::printf("%s androidauto: RFCOMM connection accepted\n", androidauto::logTimestamp().c_str());
    return connFd;
}

}  // namespace androidauto
