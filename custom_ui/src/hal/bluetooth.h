// Bluetooth HAL -- Native Linux BlueZ 5.66 Subsystem
//
// Interacts with standard Linux BlueZ 5.66 (bluetoothd / hci0) over
// D-Bus and bluetoothctl.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace hal {

struct BluetoothHandle {
    int fd = -1;
};

// Ensures the BlueZ subsystem is active (rtk_hciattach over /dev/ttyHS1 and bluetoothd).
void ensure_bluetooth_daemon_running();

// Checks whether upstream BlueZ 5.66 is active (bluetoothd or hci0)
bool is_bluez_active();

// Initializes the BlueZ Bluetooth interface (hci0).
bool init_bluetooth(BluetoothHandle & out, const char * path = nullptr);

// Starts a background state and telemetry monitoring thread.
void start_bluetooth_reader(BluetoothHandle & h);

// Registers a callback for BlueZ device and telemetry events.
void watch_bluetooth_broadcasts(std::function<void(const std::string &)> callback);

// Helper for parsing "<MAC> <Name>" or standard BlueZ device lines.
bool split_plist_entry(const std::string & entry, std::string & mac, std::string & name);
bool split_mac_and_name(const std::string & entry, std::string & mac, std::string & name);

// Adapter Power (bluetoothctl power on/off)
bool set_adapter_enabled(BluetoothHandle & h, bool enabled);

// Adapter Discoverability (bluetoothctl discoverable on/off)
bool set_discoverable(BluetoothHandle & h, bool discoverable);

// Adapter Advertised Name (bluetoothctl system-alias)
bool set_device_name(BluetoothHandle & h, const std::string & name);

// Pairing PIN / Passkey
bool set_pairing_pin(BluetoothHandle & h, const std::string & pin);

// Adapter BD_ADDR
bool get_adapter_address(BluetoothHandle & h, std::string & address);

// Enumerates all paired devices
bool list_paired_devices(BluetoothHandle & h, std::vector<std::string> & devices);

// Connects to a paired Bluetooth device by MAC address
bool connect_device(BluetoothHandle & h, const std::string & mac);

// Disconnects currently active Bluetooth profiles
bool disconnect_device(BluetoothHandle & h);

// Removes/unpairs a device from BlueZ
bool remove_paired_device(BluetoothHandle & h, const std::string & mac);

// Automatically connects to the first available paired device
bool auto_reconnect_paired_device(BluetoothHandle & h);

// Returns the MAC address of the currently connected Bluetooth device, or empty string
std::string get_connected_device_mac();

// System clock sync
bool sync_clock_from_phone(BluetoothHandle & h);

// 2026-08-20: gates aa_profile_server_loop()'s (bluetooth.cpp) hand-off
// of a phone's AA RFCOMM connection to androidauto-sidecar behind the
// existing "Auto-start phone projection" setting (AutoStartCarLink,
// see ui/staging/settings_screen.cpp) -- true (the default) hands the
// fd off immediately and requests the AA screen auto-navigate (see
// consume_aa_navigate_request() below), matching this project's
// previous (dead-since-the-BlueZ-migration) +AAPDEV=-triggered
// behavior. false stashes the connected fd here instead: the AA screen
// shows its normal not-yet-connected state, and the user's own
// "Connect" tap (ui/android_auto_screen.cpp's connect_btn_cb()) is what
// actually starts the session, via start_pending_aa_connection() below.
//
// Returns true if a phone has connected over the AA Bluetooth profile
// and its fd is waiting for the user to manually start the session --
// false once handed off (or if nothing has connected yet).
bool has_pending_aa_connection();

// Hands the pending connection's fd to androidauto-sidecar now (the
// user tapped Connect). No-op, returns false, if nothing is pending --
// safe to call unconditionally from a Connect button with no separate
// has_pending_aa_connection() check first.
bool start_pending_aa_connection();

// Edge-triggered, consumed at most once per real trigger -- same
// pattern main.cpp's AaAutoStartWatcher::consume_navigate_request()
// already established (see that class's own comment for why this
// can't just call core::navigation::push() directly from
// aa_profile_server_loop()'s own background thread: LVGL/
// core::navigation must only ever be touched from the main thread).
// main()'s own loop polls this once per iteration and pushes the
// Android Auto screen, same as it already does for the (now dead)
// AaAutoStartWatcher trigger.
bool consume_aa_navigate_request();

// Live Telemetry
struct BluetoothTelemetry {
    bool connected = false;
    int battery_level = -1;      // 0..100 (%) or 0..5, -1 if unknown
    int signal_strength = -1;    // RSSI dBm or bars, -1 if unknown
    std::string connected_device_name;
    std::string track_title;
    std::string track_artist;
    std::string track_album;
    int play_status = 0;         // 0=stopped, 1=playing, 2=paused
};

// Thread-safe query of the latest live Bluetooth telemetry
BluetoothTelemetry get_telemetry();

// Telephony & Call Control (HFP / oFono)
bool answer_call(BluetoothHandle & h);
bool hangup_call(BluetoothHandle & h);
bool dial_number(BluetoothHandle & h, const std::string & number);

// Media Transport Control (AVRCP / BlueZ MediaControl1)
bool media_play_pause(BluetoothHandle & h);
bool media_next_track(BluetoothHandle & h);
bool media_prev_track(BluetoothHandle & h);

void close_bluetooth(BluetoothHandle & h);

BluetoothHandle & shared_handle();

// Returns hardware details and active MAC of the Bluetooth adapter
std::string get_bluetooth_hardware_info();

}  // namespace hal
