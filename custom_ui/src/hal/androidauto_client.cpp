#include "hal/androidauto_client.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hal {

namespace {
// Must match sidecars/androidauto/main.cpp's kSocketPath exactly.
constexpr const char * kSocketPath = "/tmp/androidauto-sidecar.sock";
}  // namespace

AndroidAutoClient::AndroidAutoClient() = default;

AndroidAutoClient::~AndroidAutoClient() {
    disconnect();
}

void AndroidAutoClient::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool AndroidAutoClient::ensureConnected() {
    if (fd_ >= 0) return true;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    fd_ = fd;
    return true;
}

bool AndroidAutoClient::sendCommand(const std::string & line, std::string & reply) {
    std::string withNewline = line + "\n";
    if (write(fd_, withNewline.data(), withNewline.size()) !=
        static_cast<ssize_t>(withNewline.size())) {
        return false;
    }

    reply.clear();
    char buf[512];
    while (reply.find('\n') == std::string::npos) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) return false;
        reply.append(buf, static_cast<size_t>(n));
    }
    // Trim to just the first line (protocol is one line per response).
    reply.resize(reply.find('\n'));
    return true;
}

bool AndroidAutoClient::requestConnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected() && sendCommand("CONNECT", reply)) {
            return true;
        }
        disconnect();  // stale/broken connection -- retry fresh once
    }
    return false;
}

std::string AndroidAutoClient::statusLine() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected() && sendCommand("STATUS", reply)) {
            return reply;
        }
        disconnect();
    }
    return "ERR sidecar unreachable";
}

}  // namespace hal
