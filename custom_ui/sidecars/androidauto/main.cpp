// androidauto-sidecar entry point.
//
// Owns all aasdk/Boost/OpenSSL/Protobuf complexity in isolation from
// the main UI binary -- same reasoning as sidecars/carplay/main.cpp's
// own comment (isolated dependency stack, UI process free to restart
// independently). Runs androidauto::WirelessSessionManager and
// re-exposes a minimal local protocol over a Unix domain socket for
// the main UI process to consume via src/hal/androidauto_client.h.
//
// Protocol (see src/hal/androidauto_client.h for the client side):
// newline-delimited text, one request/response pair per line read.
//   "CONNECT" -> starts/restarts the wireless AA connection sequence
//                (androidauto::WirelessSessionManager::start()),
//                replies "OK"
//   "STATUS"  -> replies "STATE <state_name> <message...>"
//   "SHOW"    -> sets androidauto::video_visible() true (see
//                video_visibility.h), replies "OK". Sent by
//                ui/android_auto_screen.cpp when it becomes the active
//                screen -- decode/the session itself are NOT gated by
//                this, only whether VideoChannel actually shows the
//                hardware video layer once frames are ready.
//   "HIDE"    -> sets androidauto::video_visible() false, replies "OK"
//   "KEY <code>" -> forwards a real Android KeyEvent keycode into the
//                current session's InputChannel as a momentary tap
//                (WirelessSessionManager::sendInputKey(), see its own
//                comment), replies "OK" whether or not a session
//                currently exists to receive it (not treated as an
//                error -- matches the physical knob's own "may be
//                turned before any AA connection exists" case). "ERR
//                bad KEY command" if <code> doesn't parse as an
//                integer. Sent by hal/knob.cpp via hal/
//                androidauto_client.h whenever the physical control
//                knob is turned/pressed while the Android Auto screen
//                is the active one.
//   (anything else) -> replies "ERR unknown command"
// One thread per accepted connection (expected connection count: 2 as
// of the status-bar work in src/ui/status_bar.cpp -- android_auto_screen.cpp's
// own poll timer, plus status_bar.cpp's independent, non-spawning
// client used for the persistent top-bar connectivity glyph on every
// other screen) -- each thread services that connection's request/
// reply loop until it errors or the peer closes, matching this
// codebase's general "blocking I/O gets its own thread" convention
// (e.g. hal::McuInputHal, core::ReverseGearWatcher).
//
// NOT YET hardware-tested.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "androidauto/log_timing.h"
#include "core/log_timing.h"
#include "androidauto/video_visibility.h"
#include "androidauto/wireless_session_manager.h"

namespace {

// Must match src/hal/androidauto_client.cpp's kSocketPath exactly.
constexpr const char * kSocketPath = "/tmp/androidauto-sidecar.sock";

const char * state_name(androidauto::WirelessSessionState s) {
    switch (s) {
        case androidauto::WirelessSessionState::Idle: return "Idle";
        case androidauto::WirelessSessionState::StartingAccessPoint: return "StartingAccessPoint";
        case androidauto::WirelessSessionState::BluetoothHandshake: return "BluetoothHandshake";
        case androidauto::WirelessSessionState::WaitingForWifiJoin: return "WaitingForWifiJoin";
        case androidauto::WirelessSessionState::Connecting: return "Connecting";
        case androidauto::WirelessSessionState::Connected: return "Connected";
        case androidauto::WirelessSessionState::Failed: return "Failed";
    }
    return "Unknown";
}

void handle_connection(int clientFd, androidauto::WirelessSessionManager * manager) {
    std::string buf;
    char chunk[512];

    while (true) {
        // Read until a full line is buffered.
        size_t newlinePos;
        while ((newlinePos = buf.find('\n')) == std::string::npos) {
            ssize_t n = read(clientFd, chunk, sizeof(chunk));
            if (n <= 0) {
                close(clientFd);
                return;
            }
            buf.append(chunk, static_cast<size_t>(n));
        }

        std::string line = buf.substr(0, newlinePos);
        buf.erase(0, newlinePos + 1);

        std::string reply;
        if (line == "CONNECT") {
            manager->start();
            reply = "OK\n";
        } else if (line == "STATUS") {
            reply = std::string("STATE ") + state_name(manager->state()) + " " +
                    manager->statusMessage() + "\n";
        } else if (line == "SHOW") {
            androidauto::video_visible().store(true, std::memory_order_release);
            reply = "OK\n";
        } else if (line == "HIDE") {
            androidauto::video_visible().store(false, std::memory_order_release);
            reply = "OK\n";
        } else if (line.rfind("KEY ", 0) == 0) {
            std::string arg = line.substr(4);
            char *end = nullptr;
            long code = std::strtol(arg.c_str(), &end, 10);
            if (end == arg.c_str() || *end != '\0' || code < 0) {
                reply = "ERR bad KEY command\n";
            } else {
                manager->sendInputKey(static_cast<std::uint32_t>(code));
                reply = "OK\n";
            }
        } else {
            reply = "ERR unknown command\n";
        }

        if (write(clientFd, reply.data(), reply.size()) != static_cast<ssize_t>(reply.size())) {
            close(clientFd);
            return;
        }
    }
}

}  // namespace

int main() {
    // Literal first line -- see log_timing.h's own comment. Every log
    // line in this whole process, from here through the entire aasdk
    // session lifetime, is now on one continuous kernel-dmesg-style
    // timeline. Both clocks: androidauto::log_timing for this process's
    // own androidauto/*.cpp code, AND core::log_timing (custom_ui's own
    // instance, normally started by custom_ui's main()) -- hal_config.cpp/
    // video_layer.cpp are shared files linked into this binary too (see
    // Makefile's LOG_TIMING_CORE_OBJ) and call core::log_timestamp()
    // directly; without this, their log lines print "[    ?.??????]"
    // forever in this process (real hardware caught exactly that).
    androidauto::markProcessStart();
    core::mark_process_start();

    // 2026-08-15: found on real hardware -- alsa-lib bakes the build
    // HOST's own --with-configdir path into libasound.a at compile time
    // (statically linking it, per the earlier reversible-ALSA-rebuild
    // work, doesn't change this), so on the actual device
    // snd_pcm_open("plug:softvol2") failed with "Unknown PCM
    // plug:softvol2" -- not because that PCM/plugin doesn't exist, but
    // because alsa.conf itself (which defines what "plug"/"softvol" TYPE
    // even mean, and which in turn @hooks-loads this device's real
    // /etc/asound.conf, already present in the rootfs, to define
    // "softvol2" specifically) couldn't be found at
    // /home/osboxes/build-deps/alsa-arm-install/share/alsa/alsa.conf,
    // a path that only ever existed on the build machine. Fixed by
    // shipping that same alsa.conf (plus its own confdir includes --
    // cards/ctl/pcm subdirs) in the rootfs overlay
    // (firmware_overlay/usr/share/alsa/) and pointing
    // ALSA_CONFIG_PATH at the real on-device copy -- alsa-lib checks
    // this env var before its compiled-in default. Must be set before
    // any ALSA call the AlsaOutput class ever makes.
    setenv("ALSA_CONFIG_PATH", "/usr/share/alsa/alsa.conf", 1);

    std::printf("%s androidauto-sidecar: starting\n", androidauto::logTimestamp().c_str());

    androidauto::WirelessSessionManager manager;

    unlink(kSocketPath);  // stale socket from a previous crashed run

    int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: socket() failed: %s\n",
                     androidauto::logTimestamp().c_str(), std::strerror(errno));
        return 1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (bind(listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: bind(%s) failed: %s\n",
                     androidauto::logTimestamp().c_str(), kSocketPath, std::strerror(errno));
        return 1;
    }

    if (listen(listenFd, 4) != 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: listen() failed: %s\n",
                     androidauto::logTimestamp().c_str(), std::strerror(errno));
        return 1;
    }

    std::printf("%s androidauto-sidecar: listening on %s\n", androidauto::logTimestamp().c_str(),
                kSocketPath);

    while (true) {
        int clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "%s androidauto-sidecar: accept() failed: %s\n",
                         androidauto::logTimestamp().c_str(), std::strerror(errno));
            break;
        }
        std::thread(handle_connection, clientFd, &manager).detach();
    }

    close(listenFd);
    return 1;
}
