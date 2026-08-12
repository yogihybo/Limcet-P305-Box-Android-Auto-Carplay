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
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
    std::printf("androidauto-sidecar: starting\n");

    androidauto::WirelessSessionManager manager;

    unlink(kSocketPath);  // stale socket from a previous crashed run

    int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::fprintf(stderr, "androidauto-sidecar: socket() failed: %s\n", std::strerror(errno));
        return 1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (bind(listenFd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "androidauto-sidecar: bind(%s) failed: %s\n", kSocketPath,
                     std::strerror(errno));
        return 1;
    }

    if (listen(listenFd, 4) != 0) {
        std::fprintf(stderr, "androidauto-sidecar: listen() failed: %s\n", std::strerror(errno));
        return 1;
    }

    std::printf("androidauto-sidecar: listening on %s\n", kSocketPath);

    while (true) {
        int clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "androidauto-sidecar: accept() failed: %s\n", std::strerror(errno));
            break;
        }
        std::thread(handle_connection, clientFd, &manager).detach();
    }

    close(listenFd);
    return 1;
}
