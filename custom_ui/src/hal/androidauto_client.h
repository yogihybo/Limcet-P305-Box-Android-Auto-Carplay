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
// call): client sends "CONNECT\n", "STATUS\n", "SHOW\n", or "HIDE\n",
// server replies with exactly one line, "OK\n" or "STATE <state_name>
// <message...>\n" or "ERR <reason>\n". Connection is opened lazily on
// first use and kept
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

#include <cstdint>
#include <mutex>
#include <string>

namespace hal {

enum class TouchAction { Down, Move, Up };

void try_spawn_androidauto_sidecar();

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

    // Sends "SHOW" or "HIDE" -- controls whether the sidecar's
    // VideoChannel actually shows the decoded-frame hardware layer
    // once frames are available (see androidauto/video_visibility.h).
    // Session/decode continue regardless of this -- it only gates the
    // hardware layer's visibility, so video is ready to display
    // instantly once shown rather than needing decode to catch up.
    // ui/android_auto_screen.cpp calls this with true when it becomes
    // the active screen and false when the user navigates away, so
    // AA video only appears while that screen is actually on display
    // -- see main.cpp's AaAutoStartWatcher for why this matters now
    // that a session can auto-start in the background before the user
    // has ever opened this screen. Never spawns the sidecar (unlike
    // requestConnect()/statusLine()) -- returns false if it isn't
    // already running, since there's nothing to show/hide in that case.
    bool setVisible(bool visible);

    // Sends "FOCUS" -- returns true if the phone currently holds NATIVE
    // video focus (its own in-app exit/back control, session stays
    // Connected in the background -- see sidecars/androidauto/main.cpp's
    // own protocol comment and androidauto/video_visibility.h). Returns
    // false both for genuine PROJECTED focus and for "sidecar
    // unreachable" -- same reasoning as sendKey()/sendTouch(): nothing
    // to switch away from if there's no session to ask. Never spawns
    // the sidecar (allow_spawn=false), same as setVisible().
    bool videoFocusNative();

    // Sends "RESUME" -- asks the phone to resume PROJECTED video focus
    // after it granted itself NATIVE (its own in-app exit/back
    // control) -- see sidecars/androidauto/main.cpp's own protocol
    // comment for why this is an unsolicited grant, not a real focus
    // "request". Returns false if the sidecar can't be reached at all
    // (not whether the phone actually resumes -- poll videoFocusNative()
    // afterwards for that). Never spawns the sidecar (allow_spawn=
    // false), same as setVisible()/sendKey()/sendTouch().
    bool requestResumeVideo();

    // Sends "KEY <code>" -- forwards a real Android KeyEvent keycode
    // into the sidecar's current AA session as a momentary tap (see
    // sidecars/androidauto/main.cpp's own protocol comment and
    // androidauto::Session::sendInputKey()). Used by hal/knob.cpp to
    // forward the physical control knob's rotation/push into an active
    // session. Like setVisible(), never spawns the sidecar (allow_spawn
    // = false) -- a knob turn before any AA connection exists has
    // nothing to forward to, so there's no reason to start the aasdk-
    // backed process just to immediately no-op. Returns false if the
    // sidecar isn't reachable at all; true whether or not a session
    // currently exists to receive the key (the sidecar itself treats
    // "no session" as a normal no-op, not an error).
    bool sendKey(std::uint32_t keycode);

    // Sends "TOUCH <x> <y> <DOWN|MOVE|UP>" -- forwards a real touch
    // sample (already in the 800x480 screen-pixel space
    // Session::onServiceDiscoveryRequest advertises for this channel,
    // see hal/touch.h) into the sidecar's current AA session. Used by
    // hal/touch.cpp to forward the physical touch panel's samples (via
    // the Limcet MCU, see hal/mcu_input.h -- NOT evdev, see that file's
    // header comment) into an active session, replacing the old
    // TouchForwarder design (which read a second evdev fd that this
    // hardware never delivers real touch through at all, and which
    // couldn't have worked from the sidecar process anyway -- the MCU
    // serial port is read exclusively by custom_ui's own McuInputHal).
    // Same allow_spawn=false/best-effort semantics as sendKey().
    bool sendTouch(std::uint32_t x, std::uint32_t y, TouchAction action);

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
