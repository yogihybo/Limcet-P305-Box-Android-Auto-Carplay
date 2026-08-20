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
//   "TOUCH <x> <y> <DOWN|MOVE|UP>" -> forwards one real touch sample
//                (already in the 800x480 screen-pixel space this
//                project advertises for INPUT_SOURCE) into the current
//                session's InputChannel
//                (WirelessSessionManager::sendInputTouch()), same
//                no-session-is-fine / "ERR bad TOUCH command" contract
//                as KEY above. Sent by hal/touch.cpp via hal/
//                androidauto_client.h whenever the physical touch panel
//                (relayed by the Limcet MCU, see hal/mcu_input.h) is
//                touched while the Android Auto screen is active --
//                replaces the old TouchForwarder/evdev design, which
//                read a device node this hardware never delivers real
//                touch through at all (and which couldn't have worked
//                from THIS process anyway -- the MCU serial port is
//                read exclusively by custom_ui's own process).
//   "FOCUS"   -> replies "NATIVE" or "PROJECTED" -- the phone's own
//                real VideoFocusRequestNotification.mode() (see
//                androidauto/video_visibility.h's video_focus_native()
//                and video_channel.cpp's onVideoFocusRequest()), NOT
//                the same thing as STATUS' Connected/not-Connected: a
//                session can be fully Connected while focus is NATIVE
//                (the phone's own in-app exit/back control was used --
//                it stays connected in the background, this is not a
//                disconnect). ui/android_auto_screen.cpp polls this
//                alongside STATUS to know when to switch its own fb0/
//                LVGL layer back into view.
//   "RESUME"  -> asks the phone to resume PROJECTED video focus (see
//                WirelessSessionManager::resumeVideoFocus()/
//                VideoChannel::requestResumeFocus() for why this is an
//                unsolicited grant, not a real focus "request" --
//                aasdk has no way to send the latter), replies "OK"
//                whether or not a session exists (same no-session-is-
//                fine contract as KEY/TOUCH). Sent by
//                ui/android_auto_screen.cpp's "Resume" button, shown
//                when Connected but focus is NATIVE.
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
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
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

// 2026-08-18: real hardware showed multiple concurrent instances of
// this binary (and custom_ui, its usual launcher) accumulating during
// iterative manual testing, all fighting over the same hardware --
// /dev/fb4, the ALSA devices, and any live AA TCP session. This file's
// own socket setup made it worse than a simple duplicate: it
// unconditionally unlink()s any pre-existing socket file before
// bind()ing (a few lines below, kept -- that part is correct, it's
// what recovers from a genuinely-crashed previous run's stale socket
// file), which means a NEW instance silently steals the socket away
// from a previous one that's still very much alive, rather than being
// blocked by it -- the old instance just keeps running headless, with
// nothing able to reach it anymore, still holding onto whatever
// hardware it already opened. flock() on a separate lock file (not a
// PID file, which can go stale after a SIGKILL and then falsely block
// every future launch forever) checked before any of that: the kernel
// releases the lock automatically the instant a process exits for any
// reason, no stale-lock cleanup logic needed.
bool acquireSingleInstanceLock() {
    constexpr const char * kLockPath = "/tmp/androidauto-sidecar.lock";
    int fd = ::open(kLockPath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: open(%s) failed: %s -- continuing without a "
                     "single-instance guard\n", androidauto::logTimestamp().c_str(), kLockPath, std::strerror(errno));
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: another instance already holds %s -- refusing to "
                     "start a second one\n", androidauto::logTimestamp().c_str(), kLockPath);
        ::close(fd);
        return false;
    }
    return true;  // fd deliberately leaked -- held open for the life of the process
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
        } else if (line == "FOCUS") {
            // See video_visibility.h's own comment on video_focus_native()
            // -- separate from the session's overall Connected/not
            // state (STATUS above): the phone can request native focus
            // (its own in-app exit/back control) while staying fully
            // connected in the background, and custom_ui needs to know
            // that specifically to switch its own fb0/LVGL layer back
            // into view.
            reply = androidauto::video_focus_native().load(std::memory_order_acquire) ? "NATIVE\n" : "PROJECTED\n";
        } else if (line == "RESUME") {
            // See WirelessSessionManager::resumeVideoFocus()'s own
            // comment -- asks the phone to resume PROJECTED focus after
            // it granted itself NATIVE, since aasdk has no way to send a
            // real focus REQUEST (only grants/indications). No-op, not
            // an error, if there's no live session -- same contract as
            // KEY/TOUCH above.
            manager->resumeVideoFocus();
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
        } else if (line.rfind("TOUCH ", 0) == 0) {
            unsigned int x = 0, y = 0;
            char actionBuf[8] = {};
            int parsed = std::sscanf(line.c_str() + 6, "%u %u %7s", &x, &y, actionBuf);
            std::string actionStr(actionBuf);
            aap_protobuf::service::inputsource::message::PointerAction action;
            bool actionOk = true;
            if (actionStr == "DOWN") {
                action = aap_protobuf::service::inputsource::message::ACTION_DOWN;
            } else if (actionStr == "MOVE") {
                action = aap_protobuf::service::inputsource::message::ACTION_MOVED;
            } else if (actionStr == "UP") {
                action = aap_protobuf::service::inputsource::message::ACTION_UP;
            } else {
                actionOk = false;
                action = aap_protobuf::service::inputsource::message::ACTION_DOWN;  // unused, silences -Wmaybe-uninitialized
            }
            if (parsed != 3 || !actionOk) {
                reply = "ERR bad TOUCH command\n";
            } else {
                manager->sendInputTouch(x, y, action);
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
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

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

    // 2026-08-19: real gap found via code audit -- handle_connection()
    // (per-connection thread, spawned per client in the accept loop
    // below) writes replies to clientFd unconditionally. If custom_ui
    // ever disconnects or times out between sending a command and this
    // process replying, that write() hits a broken pipe -- and
    // SIGPIPE's default disposition is to terminate the WHOLE PROCESS,
    // not just fail the one write() call. That would kill this
    // sidecar entirely, including any live AA session, over what
    // should just be a failed reply to one client. Ignoring SIGPIPE
    // lets write() fail with EPIPE instead (already handled just below
    // -- close(clientFd); return;), the correct, contained behavior.
    // Must be set before any thread that could write to a socket
    // starts, so this is the first thing after process-start marking.
    std::signal(SIGPIPE, SIG_IGN);

    if (!acquireSingleInstanceLock()) {
        return 1;
    }

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
    // a path that only ever existed on the build machine.
    //
    // 2026-08-15 REVISED: originally shipped that same alsa.conf tree
    // in the rootfs overlay (firmware_overlay/usr/share/alsa/) and
    // pointed ALSA_CONFIG_PATH at a fixed /usr/share/alsa/alsa.conf --
    // but this project has no fixed, real rootfs install path for its
    // OWN binaries yet either (see hal/androidauto_client.cpp's
    // trySpawnSidecar() comment: "revisit once Phase 6's real firmware
    // integration lands"), so a fixed /usr/share path was one more
    // thing that could drift out of sync with wherever this binary
    // actually got copied to. Simpler and consistent with how this
    // process ALREADY gets found (custom_ui's own trySpawnSidecar()
    // resolves androidauto-sidecar's path via /proc/self/exe, assuming
    // it lives right next to custom_ui): resolve THIS process's own
    // executable directory the same way and point ALSA_CONFIG_PATH at
    // an "alsa/" subdirectory there -- Makefile's
    // $(BUILD_DIR)/alsa/alsa.conf rule stages the real config tree
    // (etc/alsa/, copied from the ALSA cross-build's own share/alsa/)
    // right alongside the compiled binaries, so scp'ing build/ to the
    // device (this project's real test workflow) carries it along
    // automatically, same as hal.conf/default_settings.conf already do.
    // alsa-lib checks this env var before its compiled-in default; must
    // be set before any ALSA call the AlsaOutput/AlsaInput classes ever
    // make. If /proc/self/exe can't be resolved (shouldn't happen on
    // Linux), ALSA_CONFIG_PATH is simply left unset and alsa-lib falls
    // back to its own compiled-in (build-host-only) default -- same
    // failure this whole fix addresses, just not expected to trigger.
    {
        char exePath[512];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            std::string dir(exePath);
            auto slash = dir.find_last_of('/');
            if (slash != std::string::npos) {
                dir.resize(slash);
                setenv("ALSA_CONFIG_PATH", (dir + "/alsa/alsa.conf").c_str(), 1);
            }
        }
    }

    // 2026-08-18: briefly bumped aasdk's own internal logger
    // (ModernLogger, see aasdk/Common/ModernLogger.hpp) from its
    // default LogLevel::INFO to DEBUG to help diagnose an ECONNRESET
    // -- reverted the same day. Real hardware showed DEBUG logs almost
    // every single I/O event (a receive()/distributeReceivedData()
    // pair for every few bytes off the wire), and on this device's
    // slow serial console the synchronous stdout writes became their
    // own head-of-line-blocking problem -- the exact same class of bug
    // the audio-ack fix above addresses, just moved into the logger.
    // Left at the default; if aasdk-internal detail is ever needed
    // again, prefer ConsoleSink -> a FileSink (see ModernLogger.hpp)
    // so it doesn't contend with the console, and/or setCategoryLevel()
    // for just the category under investigation instead of a global
    // DEBUG bump.

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
