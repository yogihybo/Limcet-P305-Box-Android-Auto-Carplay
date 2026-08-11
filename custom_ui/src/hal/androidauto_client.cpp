#include "hal/androidauto_client.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hal {

namespace {
// Must match sidecars/androidauto/main.cpp's kSocketPath exactly.
constexpr const char * kSocketPath = "/tmp/androidauto-sidecar.sock";

// Launches androidauto-sidecar if it isn't already running, so "Android
// Auto mode active" (the user opening this screen, which polls
// statusLine() immediately) is what actually starts the sidecar
// process -- no separate manual launch step, no always-on background
// service. Deliberately non-blocking: spawns and returns immediately
// rather than waiting for the socket to appear, since this runs on the
// LVGL main thread (via ensureConnected(), called from
// AndroidAutoClient's public methods) -- a multi-second blocking wait
// here would freeze the whole UI. The sidecar binds its socket almost
// immediately on startup, well under one poll interval
// (android_auto_screen.cpp polls every 500ms), so the caller just sees
// one or two "sidecar unreachable" status ticks before the next poll's
// ensureConnected() call succeeds -- no explicit wait needed.
//
// Assumes androidauto-sidecar lives in the same directory as this
// process's own binary (resolved via /proc/self/exe) -- matches the
// current dev/test workflow (both binaries copied to the device
// together, see scripts/run_on_device.sh); revisit once Phase 6's real
// firmware integration lands with a fixed install path.
void trySpawnSidecar() {
    if (std::system("pidof androidauto-sidecar >/dev/null 2>&1") == 0) {
        return;  // already running
    }

    char exePath[512];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return;
    exePath[len] = '\0';

    std::string dir(exePath);
    auto slash = dir.find_last_of('/');
    if (slash == std::string::npos) return;
    dir.resize(slash);

    std::string cmd = dir + "/androidauto-sidecar > /tmp/androidauto-sidecar.log 2>&1 &";
    std::system(cmd.c_str());
}
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

bool AndroidAutoClient::ensureConnected(bool allow_spawn) {
    if (fd_ >= 0) return true;

    if (allow_spawn) {
        trySpawnSidecar();
    }

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

std::string AndroidAutoClient::statusLine(bool allow_spawn) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(allow_spawn) && sendCommand("STATUS", reply)) {
            return reply;
        }
        disconnect();
    }
    return "ERR sidecar unreachable";
}

}  // namespace hal
