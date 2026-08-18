#include "hal/androidauto_client.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace hal {

namespace {
// Must match sidecars/androidauto/main.cpp's kSocketPath exactly.
constexpr const char * kSocketPath = "/tmp/androidauto-sidecar.sock";

// 2026-08-13: serializes trySpawnSidecar()'s check-then-spawn below
// across every AndroidAutoClient instance in this process -- each
// instance has its own mutex_ (AndroidAutoClient::mutex_), which does
// NOT protect this free function at all. Two real, unrelated callers
// exist: ui/android_auto_screen.cpp's static client() (polled every
// 500ms from the LVGL main thread) and main.cpp's AaAutoStartWatcher
// (a fresh, separate AndroidAutoClient constructed on its own
// background thread every +AAPDEV= trigger). If the auto-trigger fires
// while the user happens to have the Android Auto screen open (a
// realistic overlap -- users have been manually opening it out of
// habit while auto-start wasn't working), both could see "pidof
// androidauto-sidecar" report nothing running and each spawn their own
// copy: two processes racing to unlink/bind the same socket path,
// bring up the WiFi AP, and open /dev/bw_aap independently.
std::mutex g_spawnMutex;

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
    std::lock_guard<std::mutex> lock(g_spawnMutex);

    // 2026-08-19: statusLine() (called every 500ms from
    // android_auto_screen.cpp's poll_timer_cb, allow_spawn=true by
    // default) retries twice per call, each attempt reaching here via
    // ensureConnected() whenever fd_ < 0 -- e.g. right after the
    // sidecar crashes/gets OOM-killed. Each call below is TWO
    // std::system() forks (pidof + spawn), so an offline sidecar meant
    // up to 4 shell fork/execs every 500ms, all on the single LVGL main
    // thread, right when the system is already under the same pressure
    // that likely killed the sidecar in the first place. Rate-limited
    // to one real attempt per 5s -- the sidecar binds its socket almost
    // immediately once it does start, so this doesn't meaningfully
    // delay a legitimate respawn, it just stops the redundant retries
    // in between.
    static auto lastAttempt = std::chrono::steady_clock::time_point::min();
    auto now = std::chrono::steady_clock::now();
    if (now - lastAttempt < std::chrono::seconds(5)) {
        return;
    }
    lastAttempt = now;

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

    // No stdout/stderr redirect -- inherits this process's own fds, so
    // the sidecar's logging (wireless_session_manager.cpp,
    // bw_aap_client.cpp, etc.) lands in the same console as custom_ui's
    // own, instead of the easy-to-miss /tmp/androidauto-sidecar.log
    // this used to redirect to. Per explicit request during real
    // hardware AA connection debugging.
    std::string cmd = dir + "/androidauto-sidecar &";
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

    // 2026-08-19: real gap found via code audit -- this socket had no
    // receive timeout at all, unlike every other IPC path in this
    // codebase (hal/bluetooth.cpp, bw_aap_client.cpp, etc. all use
    // select()-based timeouts). sendCommand()'s read() below is called
    // from statusLine() (polled every 500ms by ui/android_auto_screen.cpp
    // from the LVGL main thread) and from hal/touch.cpp's sendTouch()
    // (called at the touch panel's own poll rate) -- both run on the
    // single LVGL main thread, so an unbounded block here if the
    // sidecar ever stalls (a hang anywhere in its own blocking
    // operations -- wifi_ap.sh, the bw_aap handshake waits, etc. --
    // several of which this same audit pass found real gaps in) would
    // freeze the ENTIRE UI, not just Android Auto's own screen. 1s is
    // generous for a normally-fast local IPC round trip while still
    // bounding the worst case tightly for a real-time UI thread.
    struct timeval tv {};
    tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

bool AndroidAutoClient::setVisible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    // allow_spawn=false -- SHOW/HIDE only matters once a session
    // already exists to have video from; if the sidecar isn't even
    // running, there's nothing to show, and spawning it just to tell
    // it to stay hidden would be pointless (worse, HIDE is sent from
    // android_auto_screen.cpp's own teardown path -- spawning a fresh
    // sidecar there on the way OUT of the screen would be actively
    // wrong).
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) &&
            sendCommand(visible ? "SHOW" : "HIDE", reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::videoFocusNative() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand("FOCUS", reply)) {
            return reply == "NATIVE";
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::requestResumeVideo() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand("RESUME", reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendKey(std::uint32_t keycode) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    std::string cmd = "KEY " + std::to_string(keycode);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand(cmd, reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendTouch(std::uint32_t x, std::uint32_t y, TouchAction action) {
    const char * actionStr = action == TouchAction::Down ? "DOWN" : action == TouchAction::Move ? "MOVE" : "UP";
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    std::string cmd = "TOUCH " + std::to_string(x) + " " + std::to_string(y) + " " + actionStr;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand(cmd, reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

}  // namespace hal
