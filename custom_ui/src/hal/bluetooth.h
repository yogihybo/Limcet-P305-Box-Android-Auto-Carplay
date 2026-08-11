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
// Confirmed wire framing (`BlueToothAdapter_Blueware::writeCommand()`,
// decompiled): every outgoing line is the literal template
// `"AT+%1\r\n"` with the token substituted in -- e.g. sending `SCAN=1`
// through this class actually writes `AT+SCAN=1\r\n` to the fd. No
// other framing/checksum/length prefix. Incoming lines are
// `+PREFIX=value` (also `\r\n`-terminated) -- confirmed prefixes
// include `+PLIST=`, `+ADDR=`, `+NAME=`, `+PIN=`, `+PAIRED=0`,
// `+HFPSTAT=`, `+HFPDEV=`, `+DEVSTAT=`, `+A2DPSTAT=`, `+VER=`. The
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

#include <string>
#include <vector>

namespace hal {

struct BluetoothHandle {
    int fd = -1;
};

// Starts /usr/bin/blueware (Feasycom's Bluetooth daemon -- see top
// comment) if it isn't already running (checked via `pidof blueware`).
// /dev/bw_serial doesn't exist until this daemon creates it, and
// nothing in this device's boot scripts (rc.d/rcS, inittab) starts it
// automatically -- on stock firmware it was launched by MsnCoreApp at
// runtime; since custom_ui replaces that app, nothing did this until
// now. Fire-and-forget (std::system(...) with a trailing `&`), same
// pattern as androidauto_client.cpp's trySpawnSidecar(). Safe to call
// more than once (the pidof check makes a second call a no-op) --
// init_bluetooth() below also calls this itself, so callers don't
// strictly need to call it directly, but main.cpp does anyway, early
// at startup, to give the daemon maximal time to come up before the
// first screen tries to open /dev/bw_serial.
void ensure_bluetooth_daemon_running();

// Opens /dev/bw_serial, calling ensure_bluetooth_daemon_running()
// first and retrying briefly (the daemon needs a moment after
// spawning to create the node) if it wasn't already up. Non-fatal
// pattern, same as every other optional-hardware HAL in this
// codebase -- returns false (fd stays -1) if the node still isn't
// there after retrying, logged not fatal.
bool init_bluetooth(BluetoothHandle & out, const char * path = "/dev/bw_serial");

// Sends AT+<command>\r\n (this function adds the "AT+" prefix and
// "\r\n" terminator -- pass just the token, e.g. "SCAN=1", not the
// full line) and collects whatever "+PREFIX=..." response line(s)
// arrive within timeout_ms. Returns false on write failure or if
// nothing was read before the timeout. Exposed publicly so a
// Settings/diagnostic screen can issue any of the vocabulary above
// without this class needing a dedicated wrapper for every single
// one.
bool send_command(BluetoothHandle & h, const std::string & command,
                   std::vector<std::string> & response_lines, int timeout_ms = 2000);

// BTEN=1 / BTEN=0
bool set_adapter_enabled(BluetoothHandle & h, bool enabled);

// SCAN=1 -- makes this device discoverable to phones (see this file's
// top comment: NOT an active inquiry-scan of nearby devices, despite
// the name suggesting otherwise).
bool set_discoverable(BluetoothHandle & h, bool discoverable);

// PLIST -- raw response lines, one (probable) device per line, format
// unconfirmed (see top comment).
bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices);

// HFPCONN=<mac>
bool connect_device(BluetoothHandle & h, const std::string & mac);

// No confirmed "forget" AT command exists in the source this project
// has access to -- deliberately NOT implemented. See top comment.
// bool forget_device(BluetoothHandle & h, const std::string & mac);

// NAME=<devname>
bool set_device_name(BluetoothHandle & h, const std::string & name);

// PIN=<code>
bool set_pairing_pin(BluetoothHandle & h, const std::string & pin);

// ADDR -- this adapter's own BT address, first response line.
bool get_adapter_address(BluetoothHandle & h, std::string & address);

void close_bluetooth(BluetoothHandle & h);

}  // namespace hal
