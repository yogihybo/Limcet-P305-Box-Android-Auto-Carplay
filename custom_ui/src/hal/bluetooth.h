// Bluetooth HAL -- talks to the real BT stack on this device.
//
// IMPORTANT: this is NOT the same channel as
// src/androidauto/bw_aap_client.h (`/dev/bw_aap`). That one is a
// narrow, protobuf-framed pre-connection channel Feasycom's `blueware`
// daemon exposes specifically for Android-Auto-Wireless WiFi-credential
// handoff (see docs/ARCHITECTURE.md "Wireless AA discovery"). This
// file is the GENERAL Bluetooth device-management channel a Settings
// screen needs -- scan/pair/connect/forget -- which per
// docs/WIRELESS_AND_INIT.md (`BlueToothAdapter_Blueware`, decompiled
// from the real `libBlueTooth.so`) is a plain-text AT-command protocol
// over a *different* device node, `/dev/bw_serial` (this device has
// no BlueZ/AF_BLUETOOTH stack at all, see ARCHITECTURE.md, so there
// is no `hci0`/`bluetoothd` to talk to instead). Cross-referenced
// against docs/VENDOR_BSP_RESEARCH.md section 4c
// (`ArkIVI/BusinessLogic/Bluetooth.cpp`, a second real vendor source)
// -- both agree on the vocabulary below.
//
// Full command reference: docs/BLUEWARE_AT_COMMANDS.md catalogues all
// 209 AT-command tokens found in blueware's own binary (via `strings`),
// organized by BT profile (HFP/A2DP/AVRCP/SPP/MAP/PBAP/GATT/AAP-IAP/
// etc.), each marked confirmed-tested / confirmed-broadcast-seen /
// string-only-inferred. This file's own comments below only cover the
// handful this project actually uses.
//
// Confirmed wire framing (`BlueToothAdapter_Blueware::writeCommand()`,
// decompiled): every outgoing line is the literal template
// `"AT+%1\r\n"` with the token substituted in -- e.g. sending `SCAN=1`
// through this class actually writes `AT+SCAN=1\r\n` to the fd. No
// other framing/checksum/length prefix. Incoming lines are
// `+PREFIX=value` (also `\r\n`-terminated) -- confirmed prefixes
// include `+PLIST=`, `+ADDR=`, `+NAME=`, `+PIN=`, `+PAIRED=0`,
// `+HFPSTAT=`, `+HFPDEV=`, `+DEVSTAT=`, `+A2DPSTAT=`, `+VER=`, plus
// two seen live on real hardware (2026-08-12) but NOT in either
// decompiled source: `+HFPSIG=` (unsolicited, meaning unknown -- shows
// up even with nothing paired/connected) and `+AAPSTAT=`/`+AAPDEV=`
// (also unsolicited, almost certainly Android-Auto-wireless-pairing
// discovery status -- `+AAPDEV=04006EAF29C4<sep>Pixel 9 Pro` appeared
// the moment a real AA-capable phone was nearby, `+AAPSTAT=` cycled
// 1/2/3 around it). Both are emitted independent of anything this app
// sends -- see send_command()'s own comment for why that matters. The
// stock app's own `onReadLine()` field-by-field parse of these was
// NOT fully decompiled (docs/WIRELESS_AND_INIT.md flags it as an 8.8
// KB not-yet-decompiled function) -- so this class exposes raw
// response LINES to callers rather than parsed structured fields,
// honest about what's actually confirmed vs. guessed. UI code should
// treat each line as an opaque device-list entry, not attempt to
// split out name/MAC/RSSI sub-fields.
//
// Confirmed AT-command vocabulary (outgoing tokens, both sources
// agree):
//   BTEN=1/0            adapter enable/disable
//   SCAN=1              discoverable-to-phones on (no confirmed
//                        SCAN=0/off form, nor an active-inquiry-scan-
//                        for-nearby-devices command, was recovered)
//   PLIST / PLIST=0     list paired devices
//   HFPCONN / HFPCONN=<mac>  connect HFP (bare form re-connects last
//                             device per the recovered vocabulary)
//   A2DPDISC, HFPDISC   disconnect
//   PIN=<code>          set pairing PIN
//   NAME=<devname>      set adapter's advertised name
//   ADDR                query adapter's own BT address
//   HFPSTAT, A2DPSTAT, DEVSTAT, VER  status queries
// No AT command for "forget/unpair a specific device" was found in
// either source. Practical implication for the Settings Bluetooth
// menu: pairing is phone-initiated (matches how every other AA/
// CarPlay box in this class works -- the phone finds and pairs to the
// head unit via SCAN=1 discoverability, not the reverse), and PLIST is
// what populates the "known devices" list; there's no confirmed
// "forget" command, so BluetoothChannel::forget_device() is NOT
// implemented here (see its declaration below) -- flagged as a real,
// unresolved gap, not an oversight.
//
// NOT hardware-tested by this project yet.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hal {

struct BluetoothHandle {
    int fd = -1;
};

// Starts the blueware daemon (see core/hal_config.h for the
// configurable path/properties file -- Feasycom's Bluetooth daemon,
// see top comment) if it isn't already running (checked via `pidof
// blueware`). /dev/bw_serial doesn't exist until this daemon creates
// it, and nothing in this device's boot scripts (rc.d/rcS, inittab)
// starts it automatically -- on stock firmware it was launched by
// MsnCoreApp at runtime; since custom_ui replaces that app, nothing
// did this until now. Fire-and-forget (std::system(...) with a
// trailing `&`), same pattern as androidauto_client.cpp's
// trySpawnSidecar(). Safe to call more than once (the pidof check
// makes a second call a no-op) -- init_bluetooth() below also calls
// this itself, so callers don't strictly need to call it directly, but
// main.cpp does anyway, early at startup, to give the daemon maximal
// time to come up before the first screen tries to open /dev/bw_serial.
void ensure_bluetooth_daemon_running();

// Opens the AT-command serial port (core::hal_config().bluetooth_serial_port(),
// "/dev/bw_serial" by default), calling ensure_bluetooth_daemon_running()
// first and retrying briefly (the daemon needs a moment after
// spawning to create the node) if it wasn't already up. Non-fatal
// pattern, same as every other optional-hardware HAL in this
// codebase -- returns false (fd stays -1) if the node still isn't
// there after retrying, logged not fatal.
//
// path defaults to nullptr, meaning "use the configured serial port" --
// pass an explicit path only to override it for a one-off call (e.g. a
// test/diagnostic screen probing a different node).
bool init_bluetooth(BluetoothHandle & out, const char * path = nullptr);

// Process-lifetime singleton, lazily init_bluetooth()'d on first call.
// 2026-08-12: replaces what used to be THREE separate per-translation-
// unit static handles (main.cpp had none at all -- BT was never opened
// until a screen needed it; status_bar.cpp's own bt_handle() and
// bluetooth_screen.cpp's own bt_handle() each independently opened
// their OWN fd to /dev/bw_serial the first time either was touched).
// That meant /dev/bw_serial could be opened twice concurrently, with
// two independent readers racing over the same AT-command stream --
// a real contributing factor to messy interleaved unsolicited-
// broadcast captures this project has seen. All three call sites now
// share this one handle instead.
BluetoothHandle & shared_handle();

// 2026-08-12: send_command() used to do its own one-shot ::read() per
// call, meaning unsolicited broadcasts (+AAPSTAT=/+AAPDEV=/+HFPSIG=,
// see top comment) were only ever visible if they happened to land
// inside SOME OTHER command's response window -- there was no way to
// observe them continuously. This starts a single persistent
// background reader thread that owns ALL reads on h.fd from here on;
// send_command() itself is now built on top of it (see the .cpp) via a
// shared request/dispatch queue, instead of racing a second reader on
// the same fd (which would just reintroduce the exact kind of
// interleaving bugs this project spent real effort fixing this
// session). shared_handle() calls this itself once its fd opens
// successfully -- callers don't need to call it directly unless
// they're using a BluetoothHandle NOT obtained via shared_handle()
// (uncommon; every current caller in this codebase uses the shared
// one). Idempotent -- a second call on the same handle is a no-op.
void start_bluetooth_reader(BluetoothHandle & h);

// Registers `callback` to be invoked (from the background reader
// thread started by start_bluetooth_reader() -- keep it fast and
// non-blocking, and do NOT call back into send_command()/anything
// LVGL-related directly from it) for every complete line the reader
// sees, matched to an outstanding send_command() request or not.
// `line` is the raw, unprefixed-untouched text (e.g. "+AAPDEV=
// 04006EAF29C4<sep>Pixel 9 Pro"), same shape confirmed on real
// hardware (see this file's top comment) -- callers match whatever
// prefix they care about themselves, same convention send_command()'s
// own expected_prefix parameter uses. Real use: auto-starting the
// wireless Android Auto session when a +AAPDEV= broadcast confirms a
// phone nearby has already been detected as Android-Auto-capable (see
// docs -- blueware's own AAP_ENABLE=1 feature runs the same Bluetooth
// SDP query against bonded devices Google's own AA app does, checking
// for the well-known Android Auto Wireless service UUID
// 4de17a00-52cb-11e6-bdf4-0800200c9a66; +AAPDEV= is blueware reporting
// a real match, not a guess).
void watch_bluetooth_broadcasts(std::function<void(const std::string & line)> callback);

// PLIST, then HFPCONN to the first entry if any paired device exists --
// matches this device's real factory default (FactoryConfig.ini
// [BlueTooth] AutoConnect=1, see docs/SETTINGS_REFERENCE.md section
// 2.7) which custom_ui never actually implemented: nothing previously
// called this automatically, so a device that was paired and connected
// last session stayed unconnected until a user manually opened
// Settings -> Bluetooth and tapped it. main.cpp now calls this once at
// startup, right after opening shared_handle(). Non-fatal/best-effort,
// same as every other optional-hardware path in this codebase --
// returns false if there's no handle, no paired devices, or the
// connect attempt itself fails; logs why either way.
//
// 2026-08-19: see docs/BLUETOOTH_RECONNECT_HANDOFF.md and this
// function's own .cpp comment -- now waits (bounded) for blueware's
// own +DEVSTAT=3 local-init-complete signal before the first attempt,
// and retries up to 3 times with a 2s backoff, returning true only if
// a +HFPDEV=/+AAPDEV= broadcast actually confirms the link came up
// (not just that the HFPCONN command was accepted syntactically).
bool auto_reconnect_paired_device(BluetoothHandle & h);

// Sends AT+<command>\r\n (this function adds the "AT+" prefix and
// "\r\n" terminator -- pass just the token, e.g. "SCAN=1", not the
// full line) and collects response line(s) arriving within timeout_ms.
// Returns false on write failure or if nothing was read before the
// timeout. Exposed publicly so a Settings/diagnostic screen can issue
// any of the vocabulary above without this class needing a dedicated
// wrapper for every single one.
//
// IMPORTANT, confirmed on real hardware (2026-08-12): blueware emits
// unsolicited status lines on this same serial link independent of
// anything sent to it -- e.g. `+HFPSIG=`, and (newly observed, not
// previously documented anywhere) `+AAPSTAT=`/`+AAPDEV=`, almost
// certainly Android-Auto-wireless-pairing discovery status, broadcast
// the moment an AA-capable phone is nearby. Because this function just
// collects EVERYTHING that arrives in the timeout window with no way
// to distinguish "reply to what I just sent" from "unrelated broadcast
// that happened to land in the same window," a real capture showed a
// PLIST call's response_lines polluted with an unrelated `+HFPSIG=0`
// line, which the caller (list_paired_devices(), at the time) had no
// way to tell apart from an actual device entry -- got treated as one,
// and sending it back via HFPCONN predictably failed (ERR002).
//
// `expected_prefix`, when non-empty, is the fix for exactly that: only
// lines starting with it are kept, WITH the prefix itself stripped off
// before being returned -- e.g. pass "+PLIST=" and get back only real
// device-list lines, prefix-free. Leave empty (default) for commands
// where no specific reply prefix is confirmed/expected, or where the
// caller genuinely wants the raw stream (e.g. a future diagnostic
// screen dumping whatever blueware says).
//
// 2026-08-12: this used to do its own one-shot write-then-::read()
// per call ("write, then blindly read for N ms"), which meant
// +AAPSTAT=/+AAPDEV= broadcasts arriving OUTSIDE some other call's
// timeout window were simply never seen at all -- exactly the gap this
// comment used to flag as "not fixed, since nothing today actually
// needs to observe [them]". Something now does (auto-starting the
// wireless AA session, see watch_bluetooth_broadcasts() above), so
// this is now built on top of start_bluetooth_reader()'s single
// persistent background reader + dispatch queue instead: every line
// the reader sees is checked against watch_bluetooth_broadcasts()'s
// observers AND against whichever send_command() call (if any) is
// currently outstanding, so broadcasts are visible continuously, not
// just within a command's own response window.
// silent_on_error: skips this function's own generic "adapter reported
// '<ERR>' for command '<command>'" diagnostic (still returned/left for
// the caller to inspect via response_lines as normal) -- for call
// sites where an adapter-reported error is a routine, expected outcome
// rather than something worth surfacing on every boot (e.g.
// sync_clock_from_phone()'s AT+CCLK? before any phone has connected).
// Defaults to false, preserving this diagnostic for every other
// existing caller.
bool send_command(BluetoothHandle & h, const std::string & command,
                   std::vector<std::string> & response_lines, int timeout_ms = 2000,
                   const std::string & expected_prefix = "", bool silent_on_error = false);

// Splits a device-list entry of the form "<12-hex-char MAC><1 separator
// byte><name...>" into `mac` (uppercase hex, no colons -- matches the
// exact form confirmed on real hardware in a `+AAPDEV=` broadcast,
// `04006EAF29C4` + separator + `Pixel 9 Pro`) and `name`. Returns false
// (leaving both empty) if `entry` doesn't start with 12 hex digits --
// callers should fall back to treating the whole entry as an opaque
// identifier in that case, not assume this always matches (PLIST's own
// per-entry format hasn't actually been observed yet, only inferred
// from this same vendor stack's AAPDEV shape).
bool split_mac_and_name(const std::string & entry, std::string & mac, std::string & name);

// Splits a real +PLIST= device-list entry, which (confirmed on real
// hardware 2026-08-15) is NOT the same 2-field "<mac><sep><name>" shape
// as +AAPDEV= -- it's 4 separator-delimited fields (index, a numeric
// code, MAC, name), e.g. "1<sep>16424<sep>04006EAF29C4<sep>Pixel 9
// Pro". Scans for the MAC wherever it falls (a 12-hex-digit run bounded
// by non-hex characters) rather than assuming a fixed field count/
// order. Returns false (leaving both empty) if no such run is found --
// callers should fall back to treating the whole entry as an opaque
// identifier in that case. See list_paired_devices()/connect_device().
bool split_plist_entry(const std::string & entry, std::string & mac, std::string & name);

// BTEN=1 / BTEN=0
bool set_adapter_enabled(BluetoothHandle & h, bool enabled);

// SCAN=1 -- makes this device discoverable to phones (see this file's
// top comment: NOT an active inquiry-scan of nearby devices, despite
// the name suggesting otherwise).
bool set_discoverable(BluetoothHandle & h, bool discoverable);

// PLIST -- filtered to +PLIST= lines only (prefix stripped), see
// send_command()'s comment for why that filter is load-bearing, not
// cosmetic (a real hardware capture showed an unrelated broadcast line
// polluting this list before the filter existed). Each entry's own
// internal field grammar is still unconfirmed beyond the MAC prefix --
// see split_mac_and_name().
bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices);

// HFPCONN=<mac>
bool connect_device(BluetoothHandle & h, const std::string & mac);

// No confirmed "forget" AT command exists in the source this project
// has access to -- deliberately NOT implemented. See top comment.
// bool forget_device(BluetoothHandle & h, const std::string & mac);

// A2DPDISC + HFPDISC -- disconnects the CURRENTLY ACTIVE profile
// links, not a true unpair/forget (see this file's top comment: no
// "forget a specific device" command exists in the known vocabulary).
// 2026-08-19: added to unblock ui/bluetooth_screen.cpp's "Remove
// device" button, which called a hal::disconnect_device() that never
// existed -- flagging clearly here since the button's own label
// implies permanent removal, but this can only disconnect the active
// link; the device stays paired and can still show up in PLIST /
// reconnect on its own. No specific MAC parameter -- neither AT
// command takes one in the confirmed vocabulary, so this can only
// disconnect whatever's currently connected, not target a specific
// non-active paired device from the list.
bool disconnect_device(BluetoothHandle & h);

// NAME=<devname>
bool set_device_name(BluetoothHandle & h, const std::string & name);

// PIN=<code>
bool set_pairing_pin(BluetoothHandle & h, const std::string & pin);

// ADDR -- this adapter's own BT address, first response line.
bool get_adapter_address(BluetoothHandle & h, std::string & address);

// AT+CCLK? -- queries the connected phone's own current date/time over
// this same AT-command channel (standard 3GPP TS 27.007 / Hayes clock
// command, part of the HFP AG-role vocabulary; confirmed compiled
// into this device's real blueware binary via `strings
// usr/bin/blueware`, unlike everything else in this file it's a
// standard Hayes command rather than a Feasycom-specific one, so its
// reply uses the conventional `+CCLK: "yy/MM/dd,hh:mm:ss+-zz"` framing
// -- not the `+PREFIX=value` convention documented at this file's top
// -- parsing here searches for the quoted value directly rather than
// relying on send_command()'s expected_prefix exact-match/strip
// mechanism, to stay robust to `:` vs `=` and spacing variations that
// haven't been observed on real hardware yet).
//
// 2026-08-13: this device has no RTC and no NTP client anywhere in
// its rootfs (checked -- no hwclock binary, nothing sets the clock at
// boot) -- system_clock reads near the Unix epoch on every single
// boot. Found to matter for real: androidauto/session.cpp's
// PingRequest.timestamp was leaking that "January 1970" value straight
// to the phone during a wireless AA session, a real suspect for a
// long-running silent-disconnect bug (see
// project_aa_missing_auth_complete.md memory) -- that call site now
// has its own plausibleEpochMillis() fallback regardless, but getting
// the REAL time here (once, from the actual connected phone) is the
// correct fix, not just a safety net.
//
// Best-effort, same as every other optional-hardware path in this
// codebase -- returns false (system clock left untouched) if the
// command times out, the response doesn't parse, or the connected
// device doesn't answer AT+CCLK over HFP at all (not universally
// supported -- notably some iOS versions don't reply to it; not yet
// confirmed either way for this project's real test phone). Calls
// settimeofday() directly on success -- requires the calling process
// to have permission to set the system clock (this project's
// processes already run with direct hardware access -- /dev/fb0 etc
// -- so this is consistent with everything else this codebase already
// assumes about its own privilege level).
bool sync_clock_from_phone(BluetoothHandle & h);

// 2026-08-19: one-shot diagnostic, NOT a clock-sync mechanism -- sends
// bare AT+PBDOWN and logs whatever comes back verbatim, unfiltered and
// unparsed. PBDOWN is STRING-ONLY in docs/BLUEWARE_AT_COMMANDS.md
// (present in blueware's binary, never exercised) and its own entry
// there is just "Download phonebook" -- no confirmed response format,
// no confirmed way to select the PBAP call-history folders (mch/ich/
// och) that would actually carry a real X-IRMC-CALL-DATETIME
// timestamp as opposed to plain contacts (which normally don't carry
// one at all). Deliberately does NOT call settimeofday() or touch the
// system clock -- this exists purely to find out what real hardware
// actually returns before deciding whether there's anything here worth
// building on, consistent with this project's standing preference to
// keep clock-related changes scoped to plausibleEpochMillis() rather
// than a system-wide override (see git history around f37050f).
struct BluetoothTelemetry {
    bool connected = false;
    int battery_level = -1;      // 0..100 (%) or 0..5, -1 if unknown
    int signal_strength = -1;    // 0..5 bars, -1 if unknown
    std::string connected_device_name;
    std::string track_title;
    std::string track_artist;
    std::string track_album;
    int play_status = 0;         // 0=stopped, 1=playing, 2=paused
};

// Thread-safe query of the latest live Bluetooth telemetry and state
BluetoothTelemetry get_telemetry();

// Telephony & Call Control (HFP)
bool answer_call(BluetoothHandle & h);
bool hangup_call(BluetoothHandle & h);
bool dial_number(BluetoothHandle & h, const std::string & number);

// Media Transport Control (AVRCP)
bool media_play_pause(BluetoothHandle & h);
bool media_next_track(BluetoothHandle & h);
bool media_prev_track(BluetoothHandle & h);

void close_bluetooth(BluetoothHandle & h);

}  // namespace hal
