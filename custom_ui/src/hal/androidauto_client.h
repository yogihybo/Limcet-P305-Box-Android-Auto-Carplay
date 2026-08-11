// Thin client for androidauto-sidecar's local Unix-domain-socket
// protocol -- see sidecars/androidauto/main.cpp for the server side and
// full protocol definition. Deliberately knows NOTHING about aasdk,
// Boost, or the wireless-AA handshake itself -- just a small text
// protocol over a socket, same "no libdbus/aasdk knowledge in the UI
// binary" principle docs/ARCHITECTURE.md already documents for
// carplay-sidecar. This is why src/androidauto/ (the real, heavy aasdk-
// backed session code) is no longer linked into custom_ui's own
// binary -- see Makefile's UI_TARGET comment.
//
// Protocol (newline-delimited text, one request/response pair per
// call): client sends "CONNECT\n" or "STATUS\n", server replies with
// exactly one line, "OK\n" or "STATE <state_name> <message...>\n" or
// "ERR <reason>\n". Connection is opened lazily on first use and kept
// open; reconnects automatically if the sidecar isn't running yet or
// the connection drops (e.g. sidecar restarted) -- every call is
// independently retried once against a fresh connection before giving
// up, so a sidecar that starts after the UI process doesn't need any
// particular startup ordering.
//
// androidauto-sidecar is NOT an always-on background service -- there
// is no separate manual launch step or rcS entry for it. This class
// launches it itself (see trySpawnSidecar() in the .cpp) the first
// time any method here can't connect, which in practice means
// "Android Auto mode is active" (the user opened ui/android_auto_screen.cpp,
// which polls statusLine() immediately) is what actually starts the
// process. Non-blocking -- spawns and returns immediately rather than
// waiting for the socket to appear, so this never freezes the LVGL
// main thread; the caller just sees one or two "sidecar unreachable"
// status ticks before the sidecar finishes binding its socket.
//
// NOT YET hardware-tested -- sidecars/androidauto doesn't exist as a
// running service on the device yet, this is the wiring for when it
// does.
#pragma once

#include <mutex>
#include <string>

namespace hal {

class AndroidAutoClient {
public:
    AndroidAutoClient();
    ~AndroidAutoClient();

    AndroidAutoClient(const AndroidAutoClient &) = delete;
    AndroidAutoClient & operator=(const AndroidAutoClient &) = delete;

    // Sends "CONNECT" -- tells the sidecar to (re)start the wireless AA
    // connection sequence. Returns false if the sidecar can't be
    // reached at all (not running, socket missing) -- does NOT mean
    // the AA connection itself failed, only that the request couldn't
    // be delivered; check statusLine() afterwards for real progress.
    bool requestConnect();

    // Sends "STATUS" and returns the sidecar's raw reply line
    // (e.g. "STATE Connected Android Auto session running", or
    // "ERR ..." if the sidecar itself hit an error, or a client-side
    // "ERR sidecar unreachable" if the socket couldn't be reached at
    // all -- callers don't need to distinguish these, just display the
    // line).
    //
    // `allow_spawn` (default true, ui/android_auto_screen.cpp's usage):
    // a failed connection attempt spawns the sidecar (trySpawnSidecar()
    // in the .cpp), matching the "opening the Android Auto screen is
    // what starts the sidecar" behavior documented above. Pass false
    // to only observe whatever's already running, without ever
    // starting it -- ui/status_bar.cpp does this, since a status glyph
    // that's on every screen (not just this one) launching the aasdk-
    // backed sidecar just because the home screen happens to be on
    // display would turn it into a de-facto always-on background
    // service, which the header comment above explicitly says this is
    // not.
    std::string statusLine(bool allow_spawn = true);

private:
    bool ensureConnected(bool allow_spawn = true);
    void disconnect();
    // Sends `line` (a bare command, no trailing newline needed) and
    // reads exactly one reply line. Returns false (leaving reply
    // untouched) on any I/O failure -- caller should treat this the
    // same as "sidecar unreachable".
    bool sendCommand(const std::string & line, std::string & reply);

    std::mutex mutex_;
    int fd_ = -1;
};

}  // namespace hal
