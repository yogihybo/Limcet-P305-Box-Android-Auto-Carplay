#include "hal/androidauto_client.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "core/log_timing.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <thread>
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
}  // namespace

void try_spawn_androidauto_sidecar() {
    std::lock_guard<std::mutex> lock(g_spawnMutex);

    static std::atomic<bool> s_sidecar_thread_started{false};
    if (s_sidecar_thread_started.exchange(true)) {
        return;
    }

    std::thread([]() {
        char exePath[512];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        std::string dir = ".";
        if (len > 0) {
            exePath[len] = '\0';
            std::string p(exePath);
            auto slash = p.find_last_of('/');
            if (slash != std::string::npos) {
                dir = p.substr(0, slash);
            }
        }

        std::string candidate_paths[] = {
            "/data/androidauto-sidecar",
            dir + "/androidauto-sidecar",
            "./androidauto-sidecar",
            "/usr/bin/androidauto-sidecar"
        };

        std::string sidecar_bin;
        for (const auto & path : candidate_paths) {
            struct stat st {};
            if (stat(path.c_str(), &st) == 0) {
                sidecar_bin = path;
                break;
            }
        }

        if (sidecar_bin.empty()) {
            std::printf("%s [AA-SIDECAR] Notice: androidauto-sidecar binary not found in candidate paths\n",
                        core::log_timestamp().c_str());
            return;
        }

        chmod(sidecar_bin.c_str(), 0755);
        unlink(kSocketPath);

        // Restart cleanly
        std::system("killall -9 androidauto-sidecar 2>/dev/null || true");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string cmd = sidecar_bin + " 2>&1";
        std::printf("%s [AA-SIDECAR] Launching %s (streaming logs to custom_ui console)\n",
                    core::log_timestamp().c_str(), cmd.c_str());

        FILE * fp = popen(cmd.c_str(), "r");
        if (!fp) {
            std::fprintf(stderr, "%s [AA-SIDECAR] popen failed for %s\n",
                         core::log_timestamp().c_str(), cmd.c_str());
            return;
        }

        char linebuf[512];
        while (fgets(linebuf, sizeof(linebuf), fp)) {
            size_t slen = strlen(linebuf);
            while (slen > 0 && (linebuf[slen - 1] == '\n' || linebuf[slen - 1] == '\r')) {
                linebuf[--slen] = '\0';
            }
            if (slen > 0) {
                std::printf("%s %s\n", core::log_timestamp().c_str(), linebuf);
                fflush(stdout);
            }
        }
        pclose(fp);
    }).detach();
}

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

    if (allow_spawn && std::system("pidof androidauto-sidecar >/dev/null 2>&1") != 0) {
        try_spawn_androidauto_sidecar();
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    int attempts = allow_spawn ? 10 : 1;
    for (int i = 0; i < attempts; ++i) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;

        if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
            struct timeval tv {};
            tv.tv_sec = 1;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            fd_ = fd;
            return true;
        }
        close(fd);
        if (i + 1 < attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return false;
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
