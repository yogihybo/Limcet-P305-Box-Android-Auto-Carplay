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

// 2026-08-20: hands an already-connected Bluetooth RFCOMM socket fd
// (from hal::BluezAaProfile -- see hal/bluez_aa_profile.h and
// hal/bluetooth.cpp's aa_profile_server_loop()) to androidauto-sidecar
// over their existing local socket, via SCM_RIGHTS ancillary data
// riding alongside a "CONNECT_FD" command line (see
// sidecars/androidauto/main.cpp's own protocol comment for the
// receiving side). This is the ONE place in this whole client-side
// protocol that carries a file descriptor rather than plain text, so
// it's a free function (not an AndroidAutoClient method) using its own
// short-lived dedicated connection -- deliberately NOT reusing
// AndroidAutoClient's own persistent fd_/sendCommand() machinery, which
// has no concept of ancillary data and multiplexes several unrelated
// commands (STATUS/SHOW/HIDE/KEY/TOUCH/...) over one long-lived
// connection; mixing a one-off fd-carrying message into that stream
// would risk the ancillary data landing on the wrong read() call on the
// receiving end. Spawns the sidecar if it isn't running yet (matches
// requestConnect()'s own allow_spawn=true semantics -- this IS the
// "Android Auto mode active" moment now, arguably more so than the old
// CONNECT).
//
// Always takes ownership of rfcommFd -- closes it (locally) once
// sendmsg() has handed it off to the kernel, on every path (success or
// failure), so callers never need to close it themselves. Returns true
// only if the sidecar actually replied "OK" (meaning
// WirelessSessionManager::start() was called with the fd); false on
// any failure to reach the sidecar, dispatch the message, or a
// non-"OK" reply -- callers should treat false as "this connection
// attempt didn't make it to a session," not necessarily fatal (a phone
// can, and will, dial in again).
bool sendConnectFd(int rfcommFd);

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
    // into the sidecar's current AA session as a momentary tap.
    bool sendKey(std::uint32_t keycode);

    // Sends "ROTARY <ticks>" -- forwards native automotive rotary wheel
    // relative events (KEYCODE_ROTARY_CONTROLLER delta) for smooth intra-
    // container scrolling and focus traversal.
    bool sendRotary(int ticks);

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

    // 2026-08-21: Sends "NIGHT <0|1>" -- forwards the MCU headlight-
    // driven night-mode state (see hal/mcu_input.h's own
    // get_night_mode()) into the sidecar's current AA session, where
    // sidecars/androidauto/main.cpp's own "NIGHT <0|1>" handler calls
    // androidauto::WirelessSessionManager::sendNightMode(), which
    // updates SensorChannel's SENSOR_NIGHT_MODE state (sends a fresh
    // NightModeData event immediately if the phone already subscribed
    // to that sensor, matching DRIVING_STATUS_DATA's own "send at least
    // one indication" pattern -- see sensor_channel.cpp). Same
    // allow_spawn=false/best-effort semantics as sendKey()/sendTouch()
    // -- a headlight change before any AA connection exists has nothing
    // to forward to yet; whenever a session does start,
    // Session::onServiceDiscoveryRequest/SensorChannel's own initial
    // state (see that class's own header comment on this) covers the
    // catch-up case.
    bool sendNightMode(bool nightMode);

    // Sends "EQ <bass_db> <mid_db> <treble_db> <loudness:0|1>"
    bool sendEq(int bass_db, int mid_db, int treble_db, bool loudness);

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
