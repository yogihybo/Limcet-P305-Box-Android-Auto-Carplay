#include "hal/androidauto_client.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "core/log_timing.h"
#include "core/sized_thread.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
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

    core::SizedThread(core::kDefaultThreadStackSize, []() {
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
            std::printf("%s [AA:SIDECAR] Notice: androidauto-sidecar binary not found in candidate paths\n",
                        core::log_timestamp().c_str());
            s_sidecar_thread_started.store(false, std::memory_order_release);
            return;
        }

        chmod(sidecar_bin.c_str(), 0755);
        unlink(kSocketPath);

        // Restart cleanly
        std::system("killall -9 androidauto-sidecar 2>/dev/null || true");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string cmd = sidecar_bin + " 2>&1";
        std::printf("%s [AA:SIDECAR] Launching %s (streaming logs to custom_ui console)\n",
                    core::log_timestamp().c_str(), cmd.c_str());

        FILE * fp = popen(cmd.c_str(), "r");
        if (!fp) {
            std::fprintf(stderr, "%s [AA:SIDECAR] popen failed for %s\n",
                         core::log_timestamp().c_str(), cmd.c_str());
            s_sidecar_thread_started.store(false, std::memory_order_release);
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
        // 2026-09-05: real hardware bug found via code review -- this
        // flag was set true once and never reset. If androidauto-sidecar
        // ever exits/crashes/gets OOM-killed, fgets() returns EOF, this
        // reader loop ends, pclose() returns -- and every future call to
        // try_spawn_androidauto_sidecar() (a tapped "Connect" button, a
        // freshly re-plugged/re-paired phone) returned immediately
        // without ever launching a new instance, permanently. Android
        // Auto stayed dead until the whole custom_ui process itself
        // restarted. Resetting here lets the NEXT genuine spawn attempt
        // actually spawn.
        s_sidecar_thread_started.store(false, std::memory_order_release);
    }).detach();
}

bool sendConnectFd(int rfcommFd) {
    std::printf("%s [BT] sendConnectFd: handing rfcommFd=%d to androidauto-sidecar\n",
                core::log_timestamp().c_str(), rfcommFd);

    if (std::system("pidof androidauto-sidecar >/dev/null 2>&1") != 0) {
        std::printf("%s [BT] sendConnectFd: androidauto-sidecar not running yet, "
                    "spawning it\n", core::log_timestamp().c_str());
        try_spawn_androidauto_sidecar();
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    // A freshly-spawned sidecar needs a moment to bind its socket --
    // same 10-attempt/50ms-apart retry AndroidAutoClient::ensureConnected()
    // already uses for the same reason.
    int connFd = -1;
    for (int attempt = 0; attempt < 10; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) break;
        if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
            connFd = fd;
            break;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (connFd < 0) {
        std::fprintf(stderr, "%s [BT] sendConnectFd: could not reach "
                     "androidauto-sidecar's socket -- closing rfcommFd=%d, dropping this "
                     "connection\n", core::log_timestamp().c_str(), rfcommFd);
        close(rfcommFd);
        return false;
    }

    // One sendmsg() carrying both the "CONNECT_FD\n" text and the
    // SCM_RIGHTS ancillary data -- see sidecars/androidauto/main.cpp's
    // recv_chunk() comment for why this must arrive as a single
    // message, not two separate writes.
    const char * line = "CONNECT_FD\n";
    struct iovec iov {};
    iov.iov_base = const_cast<char *>(line);
    iov.iov_len = std::strlen(line);

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char control[CMSG_SPACE(sizeof(int))];
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &rfcommFd, sizeof(int));

    ssize_t sent = sendmsg(connFd, &msg, 0);
    // rfcommFd's kernel-level ownership is transferred to the receiving
    // process as soon as sendmsg() succeeds (a dup, not a move -- our
    // own copy of the fd number is still valid until we close it, but
    // there's nothing left for us to do with it) -- close our copy
    // either way, success or failure, so this function never leaks it.
    close(rfcommFd);
    if (sent != static_cast<ssize_t>(iov.iov_len)) {
        std::fprintf(stderr, "%s [BT] sendConnectFd: sendmsg() failed (sent=%zd)\n",
                     core::log_timestamp().c_str(), sent);
        close(connFd);
        return false;
    }

    struct timeval tv {};
    tv.tv_sec = 3;
    setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // 2026-08-21: SO_RCVTIMEO alone only bounds read() -- sendmsg()
    // above (and any plain write()) has no timeout protection from it
    // at all. On a SOCK_STREAM AF_UNIX socket, if the sidecar's own
    // receive buffer fills up (e.g. its io_service thread is wedged
    // and never calling recv()), a later write/sendmsg can block this
    // process's caller indefinitely once the kernel socket buffer is
    // full -- a real, previously-unprotected path to freezing whatever
    // thread called this (see AndroidAutoClient::sendCommand()'s own
    // comment for the same gap and fix, found chasing a real hardware
    // hang with a wedged sidecar and zero log output).
    setsockopt(connFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string reply;
    char buf[256];
    while (reply.find('\n') == std::string::npos) {
        ssize_t n = read(connFd, buf, sizeof(buf));
        if (n <= 0) break;
        reply.append(buf, static_cast<size_t>(n));
    }
    close(connFd);

    bool ok = reply.rfind("OK", 0) == 0;
    std::printf("%s [BT] sendConnectFd: sidecar reply: '%s' (%s)\n",
                core::log_timestamp().c_str(), reply.c_str(), ok ? "accepted" : "rejected/unreachable");
    return ok;
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
    // A stale connection's leftover bytes (if any) must never be
    // mistaken for the start of a fresh connection's first reply.
    readBuffer_.clear();
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
            // 2026-08-21: SO_RCVTIMEO alone only bounds read() in
            // sendCommand() below -- its write() call had no timeout
            // protection at all. If the sidecar's own receive buffer
            // fills up (its io_service thread wedged, never calling
            // recv()), write() can block the calling thread
            // indefinitely once the kernel socket buffer is full --
            // every caller here runs on the LVGL main thread, so this
            // was a real path to freezing the whole UI with zero log
            // output (stuck before ever reaching a printf), found
            // chasing a real hardware hang report.
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

    // 2026-09-05: see readBuffer_'s own header comment -- reads
    // accumulate into the PERSISTENT member buffer (which may already
    // hold leftover bytes from a previous call's read() that returned
    // more than one line), not a local one that's discarded whole.
    char buf[512];
    while (readBuffer_.find('\n') == std::string::npos) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) return false;
        readBuffer_.append(buf, static_cast<size_t>(n));
    }
    // First line is this command's reply; anything after its newline
    // stays in readBuffer_ for the NEXT sendCommand() call to consume,
    // instead of being discarded.
    size_t nl = readBuffer_.find('\n');
    reply = readBuffer_.substr(0, nl);
    readBuffer_.erase(0, nl + 1);
    return true;
}

bool AndroidAutoClient::sendInput(const std::string & line) {
    std::string withNewline = line + "\n";
    if (write(fd_, withNewline.data(), withNewline.size()) !=
        static_cast<ssize_t>(withNewline.size())) {
        return false;
    }
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
    std::string cmd = "KEY " + std::to_string(keycode);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendInput(cmd)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendRotary(int ticks) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cmd = "ROTARY " + std::to_string(ticks);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendInput(cmd)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendTouch(std::uint32_t x, std::uint32_t y, TouchAction action) {
    const char * actionStr = action == TouchAction::Down ? "DOWN" : action == TouchAction::Move ? "MOVE" : "UP";
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cmd = "TOUCH " + std::to_string(x) + " " + std::to_string(y) + " " + actionStr;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendInput(cmd)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendNightMode(bool nightMode) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    std::string cmd = std::string("NIGHT ") + (nightMode ? "1" : "0");
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand(cmd, reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::requestAudioFocus(bool gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    std::string cmd = std::string("AUDIOFOCUS ") + (gain ? "1" : "0");
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand(cmd, reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

bool AndroidAutoClient::sendEq(int bass_db, int mid_db, int treble_db, bool loudness) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string reply;
    std::string cmd = "EQ " + std::to_string(bass_db) + " " + std::to_string(mid_db) + " "
                      + std::to_string(treble_db) + " " + (loudness ? "1" : "0");
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (ensureConnected(/*allow_spawn=*/false) && sendCommand(cmd, reply)) {
            return true;
        }
        disconnect();
    }
    return false;
}

namespace {

// See AndroidAutoStatusSnapshot's own header comment for why this
// exists -- one shared poller instead of 3 independent ones.
struct SharedStatusCache {
    std::mutex mtx;
    AndroidAutoStatusSnapshot snapshot;
};

SharedStatusCache & shared_status_cache() {
    static SharedStatusCache c;
    return c;
}

std::atomic<bool> g_statusPollAllowSpawn{false};
std::atomic<bool> g_statusPollStarted{false};

void status_poll_loop() {
    // Dedicated instance/socket, separate from any UI-owned
    // AndroidAutoClient (e.g. one used for one-off actions like
    // requestConnect()) -- this one's sole job is the shared poll.
    AndroidAutoClient client;
    while (true) {
        bool allow_spawn = g_statusPollAllowSpawn.load(std::memory_order_acquire);
        std::string line = client.statusLine(allow_spawn);
        bool native = false;
        if (line.rfind("STATE Connected", 0) == 0) {
            native = client.videoFocusNative();
        }
        {
            std::lock_guard<std::mutex> lock(shared_status_cache().mtx);
            shared_status_cache().snapshot.status_line = std::move(line);
            shared_status_cache().snapshot.native_focus = native;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

}  // namespace

AndroidAutoStatusSnapshot cached_android_auto_status() {
    if (!g_statusPollStarted.exchange(true)) {
        core::SizedThread(core::kDefaultThreadStackSize, status_poll_loop).detach();
    }
    std::lock_guard<std::mutex> lock(shared_status_cache().mtx);
    return shared_status_cache().snapshot;
}

void set_android_auto_status_poll_allow_spawn(bool allow) {
    g_statusPollAllowSpawn.store(allow, std::memory_order_release);
}

}  // namespace hal
