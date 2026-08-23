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
// newline-delimited text, one request/response pair per line read
// (recv'd via recvmsg(), not plain read(), specifically so CONNECT_FD's
// ancillary fd survives -- see recv_chunk()'s own comment below).
//   "CONNECT_FD" -> MUST be sent together with an SCM_RIGHTS ancillary
//                fd carrying an already-connected Bluetooth RFCOMM
//                socket (see hal::androidauto_client's sendConnectFd(),
//                the sender side) -- starts the wireless AA connection
//                sequence using that fd
//                (androidauto::WirelessSessionManager::start(fd)),
//                replies "OK", or "ERR no fd received" if the ancillary
//                data didn't arrive. This process owns NO Bluetooth/
//                D-Bus/BlueZ knowledge at all -- see
//                wireless_session_manager.h's own header comment for
//                the full architecture (custom_ui's hal::BluezAaProfile
//                does that, continuously, independent of this
//                process's lifecycle).
//   "CONNECT" -> accepted as an informational no-op for backward
//                compatibility (logged clearly as such) -- a session
//                now starts automatically via CONNECT_FD the instant a
//                phone dials in, not on any explicit request.
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
//   "NIGHT <0|1>" -> forwards custom_ui's own MCU-headlight-driven
//                night-mode state (WirelessSessionManager::
//                sendNightMode()) into the current session's
//                SensorChannel, which reports SENSOR_NIGHT_MODE to the
//                phone. Same no-session-is-fine / "ERR bad NIGHT
//                command" contract as KEY/TOUCH above.
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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "androidauto/log_timing.h"
#include "core/log_timing.h"
#include "core/sized_thread.h"
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

// 2026-08-20: reads via recvmsg() (not plain read()) so an SCM_RIGHTS
// ancillary fd riding alongside a "CONNECT_FD" line (see
// hal::androidauto_client's sendConnectFd(), the sender side) is
// captured. recvmsg() behaves identically to read() when no ancillary
// data is present, so this is a safe drop-in replacement for every
// other command on this connection too, not just CONNECT_FD -- no need
// for two different read paths. *outFd is set to the fd received on
// THIS call (-1 if none); the caller is responsible for it once
// CONNECT_FD's line is actually parsed.
ssize_t recv_chunk(int clientFd, char * buf, size_t bufLen, int * outFd) {
    *outFd = -1;
    struct msghdr msg {};
    struct iovec iov {};
    iov.iov_base = buf;
    iov.iov_len = bufLen;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char control[CMSG_SPACE(sizeof(int))];
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(clientFd, &msg, 0);
    if (n <= 0) {
        return n;
    }

    for (struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            std::memcpy(outFd, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    return n;
}

void handle_connection(int clientFd, androidauto::WirelessSessionManager * manager) {
    std::string buf;
    char chunk[512];
    int pendingFd = -1;  // captured from an SCM_RIGHTS ancillary message, if any, until consumed

    while (true) {
        // Read until a full line is buffered.
        size_t newlinePos;
        while ((newlinePos = buf.find('\n')) == std::string::npos) {
            int recvdFd = -1;
            ssize_t n = recv_chunk(clientFd, chunk, sizeof(chunk), &recvdFd);
            if (n <= 0) {
                close(clientFd);
                if (pendingFd >= 0) close(pendingFd);
                return;
            }
            if (recvdFd >= 0) {
                std::printf("%s androidauto-sidecar: received ancillary fd=%d on this connection\n",
                            androidauto::logTimestamp().c_str(), recvdFd);
                pendingFd = recvdFd;
            }
            buf.append(chunk, static_cast<size_t>(n));
        }

        std::string line = buf.substr(0, newlinePos);
        buf.erase(0, newlinePos + 1);

        std::string reply;
        if (line == "CONNECT_FD") {
            std::printf("%s androidauto-sidecar: CONNECT_FD received (fd=%d)\n",
                        androidauto::logTimestamp().c_str(), pendingFd);
            if (pendingFd < 0) {
                std::fprintf(stderr, "%s androidauto-sidecar: CONNECT_FD arrived with no ancillary "
                             "fd attached -- ignoring\n", androidauto::logTimestamp().c_str());
                reply = "ERR no fd received\n";
            } else {
                manager->start(pendingFd);
                pendingFd = -1;  // ownership handed to WirelessSessionManager::start()
                reply = "OK\n";
            }
        } else if (line == "CONNECT") {
            // 2026-08-20: this process no longer owns any Bluetooth/
            // BlueZ connectivity -- see wireless_session_manager.h's
            // own header comment. custom_ui's own hal::BluezAaProfile
            // stays registered and listens for AA RFCOMM connections
            // continuously, independent of this command; a session now
            // starts automatically (via CONNECT_FD, above) the instant
            // a phone actually dials in, not on any explicit request
            // from here. Kept as an accepted no-op (not "ERR unknown
            // command") purely for backward compatibility with any
            // caller still sending it -- logged clearly so it's obvious
            // in the console this isn't doing anything anymore.
            std::printf("%s androidauto-sidecar: CONNECT received -- informational no-op now, "
                        "Bluetooth is custom_ui's job (see CONNECT_FD)\n",
                        androidauto::logTimestamp().c_str());
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
        } else if (line.rfind("NIGHT ", 0) == 0) {
            // 2026-08-21: forwards custom_ui's own MCU-headlight-driven
            // night-mode state -- see hal/androidauto_client.h's own
            // sendNightMode() comment for the full cross-process chain.
            // Same no-session-is-fine contract as KEY/TOUCH above.
            std::string arg = line.substr(6);
            if (arg == "0" || arg == "1") {
                manager->sendNightMode(arg == "1");
                reply = "OK\n";
            } else {
                reply = "ERR bad NIGHT command\n";
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

    // 2026-08-21: see custom_ui's own main.cpp for the full comment on
    // why -- this process is at least as exposed to the same page-cache-
    // thrashing-without-swap mechanism (no swap on this 173MB device,
    // this binary is statically linked so its own text segment is the
    // thing at risk of eviction+refault from the real USB-backed
    // rootfs), and it's the process actually driving the AA session
    // that was observed dying with ECONNRESET after several minutes of
    // runtime. Best-effort/non-fatal, same reasoning as custom_ui.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "%s androidauto-sidecar: mlockall() failed: %s -- continuing "
                     "without it (process pages may still be reclaimed under memory pressure)\n",
                     androidauto::logTimestamp().c_str(), std::strerror(errno));
    } else {
        std::printf("%s androidauto-sidecar: mlockall(MCL_CURRENT|MCL_FUTURE) succeeded -- "
                    "process pages pinned against reclaim\n", androidauto::logTimestamp().c_str());
    }

    // 2026-08-15: real hw fix for the OLD static-libalsa.a build --
    // alsa-lib baked the build HOST's own --with-configdir path in at
    // compile time, so snd_pcm_open("plug:softvol2") failed with
    // "Unknown PCM plug:softvol2" because alsa.conf itself couldn't be
    // found on the device at all. Worked around by pointing
    // ALSA_CONFIG_PATH at a bundled alsa.conf staged next to the
    // binaries.
    //
    // 2026-08-24: OBSOLETE, removed -- this session's dynamic-linking
    // migration now links against Buildroot's own real, correctly
    // target-built alsa-lib (confirmed via `strings` on the actual
    // libasound.so: compiled-in ALSA_CONFIG_DIR is a real
    // /usr/share/alsa, not a build-host-only path). Forcing
    // ALSA_CONFIG_PATH at the old bundled (and by now stale)
    // build/alsa/alsa.conf actively overrides that correct default with
    // a leftover from the static-build era instead of just fixing the
    // real gap (this rootfs's own /etc/asound.conf, staged separately --
    // see firmware_overlay_dyn/etc/asound.conf) -- removed rather than
    // left as silent dead weight.

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

    // 2026-08-20: no longer auto-starts a listen-for-Bluetooth-
    // connection sequence at boot -- there's no Bluetooth for this
    // process to listen with anymore (see wireless_session_manager.h's
    // own header comment). custom_ui's own hal::BluezAaProfile does
    // that continuously, independent of this process's lifecycle, and
    // hands a connected fd to CONNECT_FD (handle_connection(), above)
    // whenever a phone actually dials in. This manager instance just
    // sits idle until that happens.
    androidauto::WirelessSessionManager manager;

    while (true) {
        int clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "%s androidauto-sidecar: accept() failed: %s\n",
                         androidauto::logTimestamp().c_str(), std::strerror(errno));
            break;
        }
        core::SizedThread(core::kDefaultThreadStackSize, handle_connection, clientFd, &manager).detach();
    }

    close(listenFd);
    return 1;
}
