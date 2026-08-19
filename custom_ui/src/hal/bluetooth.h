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

// System clock sync
bool sync_clock_from_phone(BluetoothHandle & h);

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

}  // namespace hal
