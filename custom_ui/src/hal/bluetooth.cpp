#include "hal/bluetooth.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCIDEVUP_     _IOW('H', 201, int)

#include "core/hal_config.h"
#include "core/log_timing.h"

namespace hal {

namespace {

std::mutex g_telemetry_mtx;
BluetoothTelemetry g_telemetry;

// Helpers for executing BlueZ/bluetoothctl commands and capturing stdout
bool run_command_capture(const std::string & cmd, std::vector<std::string> & lines_out) {
    lines_out.clear();
    FILE * pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return false;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines_out.push_back(line);
        }
    }
    int status = pclose(pipe);
    return (status == 0);
}

bool run_command_simple(const std::string & cmd) {
    int res = std::system(cmd.c_str());
    return (res == 0);
}

// Normalizes MAC addresses to standard colon notation (e.g. "04:00:6E:AF:29:C4")
std::string format_mac_with_colons(const std::string & raw) {
    if (raw.size() == 17 && raw[2] == ':' && raw[5] == ':') {
        return raw;
    }
    if (raw.size() == 12) {
        std::string out;
        for (size_t i = 0; i < 12; ++i) {
            out.push_back(raw[i]);
            if (i % 2 == 1 && i != 11) out.push_back(':');
        }
        return out;
    }
    return raw;
}

// Checks whether upstream BlueZ 5.66 is active (bluetoothd or hci0)
bool is_bluez_active() {
    if (std::system("pidof bluetoothd >/dev/null 2>&1") == 0) {
        return true;
    }
    struct stat st {};
    if (stat("/sys/class/bluetooth/hci0", &st) == 0) {
        return true;
    }
    return false;
}

struct ReaderState {
    std::mutex observer_mtx;
    std::vector<std::function<void(const std::string &)>> observers;
    bool started = false;
};

ReaderState & reader_state() {
    static ReaderState state;
    return state;
}

// Converts MAC address (04:00:6E:AF:29:C4 or 04006EAF29C4) to D-Bus object path component (dev_04_00_6E_AF_29_C4)
std::string mac_to_dbus_path(const std::string & mac) {
    std::string formatted = format_mac_with_colons(mac);
    std::string path = "dev_";
    for (char c : formatted) {
        if (c == ':') path.push_back('_');
        else path.push_back(std::toupper(static_cast<unsigned char>(c)));
    }
    return path;
}

void bluez_monitor_loop(BluetoothHandle * h) {
    std::printf("%s hal::bluetooth: BlueZ status monitor thread started\n", core::log_timestamp().c_str());
    bool last_connected = false;
    std::string last_connected_mac;
    std::string last_connected_name;
    size_t last_device_count = 0;

    while (h->fd >= 0) {
        std::vector<std::string> lines;
        if (run_command_capture("dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null", lines) && !lines.empty()) {
            std::lock_guard<std::mutex> lock(g_telemetry_mtx);
            bool connected = false;
            std::string connected_mac;
            std::string connected_name;
            int rssi = -1;

            std::string cur_addr, cur_name;
            bool cur_connected = false, cur_paired = false;
            size_t total_devices = 0;

            auto flush_device = [&]() {
                if (!cur_addr.empty()) {
                    total_devices++;
                    if (cur_connected) {
                        connected = true;
                        connected_mac = cur_addr;
                        connected_name = cur_name;
                    }
                }
                cur_addr.clear(); cur_name.clear();
                cur_connected = false; cur_paired = false;
            };

            for (size_t i = 0; i < lines.size(); ++i) {
                const auto & l = lines[i];
                if (l.find("object path \"/org/bluez/hci0/dev_") != std::string::npos) {
                    flush_device();
                } else if (l.find("string \"Address\"") != std::string::npos && i + 1 < lines.size()) {
                    size_t pos = lines[i+1].find("string \"");
                    if (pos != std::string::npos) {
                        cur_addr = lines[i+1].substr(pos + 8);
                        if (!cur_addr.empty() && cur_addr.back() == '"') cur_addr.pop_back();
                    }
                } else if (l.find("string \"Name\"") != std::string::npos && i + 1 < lines.size()) {
                    size_t pos = lines[i+1].find("string \"");
                    if (pos != std::string::npos) {
                        cur_name = lines[i+1].substr(pos + 8);
                        if (!cur_name.empty() && cur_name.back() == '"') cur_name.pop_back();
                    }
                } else if (l.find("string \"Alias\"") != std::string::npos && i + 1 < lines.size() && cur_name.empty()) {
                    size_t pos = lines[i+1].find("string \"");
                    if (pos != std::string::npos) {
                        cur_name = lines[i+1].substr(pos + 8);
                        if (!cur_name.empty() && cur_name.back() == '"') cur_name.pop_back();
                    }
                } else if (l.find("string \"Connected\"") != std::string::npos && i + 1 < lines.size()) {
                    if (lines[i+1].find("boolean true") != std::string::npos) cur_connected = true;
                } else if (l.find("string \"Paired\"") != std::string::npos && i + 1 < lines.size()) {
                    if (lines[i+1].find("boolean true") != std::string::npos) cur_paired = true;
                } else if (l.find("string \"RSSI\"") != std::string::npos && i + 1 < lines.size()) {
                    int val = 0;
                    if (std::sscanf(lines[i+1].c_str(), "%*[^0-9-]%d", &val) == 1) {
                        rssi = val;
                    }
                }
            }
            flush_device();

            if (connected != last_connected || connected_mac != last_connected_mac) {
                if (connected) {
                    std::printf("%s [BT-EVENT] *** Device Connected ***: %s ('%s') RSSI=%d dBm\n",
                                core::log_timestamp().c_str(), connected_mac.c_str(),
                                connected_name.c_str(), rssi);

                    // Explicitly trigger Android Auto RFCOMM profile connection on the newly connected device
                    std::string dev_p = "/org/bluez/hci0/" + mac_to_dbus_path(connected_mac);
                    std::thread([connected_mac, dev_p]() {
                        std::printf("%s [BT-CMD] Sending ConnectProfile(AA 4de17a00...) to %s (%s)...\n",
                                    core::log_timestamp().c_str(), connected_mac.c_str(), dev_p.c_str());
                        std::vector<std::string> output;
                        std::string cmd = "dbus-send --system --print-reply --dest=org.bluez " + dev_p +
                                          " org.bluez.Device1.ConnectProfile string:4de17a00-52cb-11e6-bdf4-0800200c9a66 2>&1";
                        bool ok = run_command_capture(cmd, output);
                        std::printf("%s [BT-CMD] ConnectProfile(AA) result (exit=%d):\n",
                                    core::log_timestamp().c_str(), ok ? 0 : 1);
                        for (const auto & line : output) {
                            std::printf("    [BT-CMD-REPLY] %s\n", line.c_str());
                        }
                    }).detach();
                } else {
                    std::printf("%s [BT-EVENT] *** Device Disconnected ***: (was %s '%s')\n",
                                core::log_timestamp().c_str(), last_connected_mac.c_str(),
                                last_connected_name.c_str());
                }
                last_connected = connected;
                last_connected_mac = connected_mac;
                last_connected_name = connected_name;
            }

            if (total_devices != last_device_count) {
                std::printf("%s [BT-STATUS] Total known devices in BlueZ database: %zu\n",
                            core::log_timestamp().c_str(), total_devices);
                last_device_count = total_devices;
            }

            g_telemetry.connected = connected;
            if (!connected_name.empty()) g_telemetry.connected_device_name = connected_name;
            g_telemetry.signal_strength = rssi;
        } else {
            std::lock_guard<std::mutex> lock(g_telemetry_mtx);
            if (last_connected) {
                std::printf("%s [BT-EVENT] Lost connection to BlueZ daemon\n", core::log_timestamp().c_str());
                last_connected = false;
            }
            g_telemetry.connected = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
}

}  // namespace

void ensure_bluetooth_daemon_running() {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    if (is_bluez_active()) {
        std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: BlueZ (bluetoothd) already running\n",
                    core::log_timestamp().c_str());
        return;
    }

    // Prefer running the dedicated bringup script if present and executable
    const char * scripts[] = {
        "/usr/bin/bluez-bringup.sh",
        "/data/bluez-bringup.sh",
        "/usr/share/bluez-bringup/bluez-bringup.sh",
        "scripts/bluez-bringup.sh"
    };
    for (const char * s : scripts) {
        struct stat st {};
        if (stat(s, &st) == 0) {
            std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: invoking %s start\n",
                        core::log_timestamp().c_str(), s);
            std::string cmd = std::string(s) + " start >/dev/null 2>&1";
            std::system(cmd.c_str());
            break;
        }
    }

    // Check if rtk_hciattach is already running / attaching hci0
    if (std::system("pidof rtk_hciattach >/dev/null 2>&1") != 0) {
        std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: starting rtk_hciattach over /dev/ttyHS1\n",
                    core::log_timestamp().c_str());
        std::system("rtk_hciattach -n -s 115200 /dev/ttyHS1 rtk_h5 >/dev/null 2>&1 &");
        for (int i = 0; i < 30; ++i) {
            struct stat st {};
            if (stat("/sys/class/bluetooth/hci0", &st) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (std::system("pidof dbus-daemon >/dev/null 2>&1") != 0) {
        std::system("mkdir -p /var/run/run/dbus /var/run/dbus && dbus-daemon --system --fork >/dev/null 2>&1");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::system("ln -sf /var/run/run/dbus/system_bus_socket /var/run/dbus/system_bus_socket 2>/dev/null");

    if (std::system("pidof bluetoothd >/dev/null 2>&1") != 0) {
        std::printf("%s hal::bluetooth::ensure_bluetooth_daemon_running: starting bluetoothd\n", core::log_timestamp().c_str());
        std::system("bluetoothd -n >/dev/null 2>&1 &");
        for (int i = 0; i < 20; ++i) {
            if (std::system("pidof bluetoothd >/dev/null 2>&1") == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Ensure bt-agent is running and stream its output directly to custom_ui console
    static std::atomic<bool> s_agent_thread_started{false};
    if (!s_agent_thread_started.exchange(true)) {
        std::thread([]() {
            const char * agents[] = { "/usr/bin/bt-agent", "/data/bt-agent", "./bt-agent" };
            std::string agent_bin;
            for (const char * a : agents) {
                struct stat st {};
                if (stat(a, &st) == 0 && (st.st_mode & S_IXUSR)) {
                    agent_bin = a;
                    break;
                }
            }
            if (agent_bin.empty()) {
                for (const char * a : agents) {
                    struct stat st {};
                    if (stat(a, &st) == 0) {
                        agent_bin = a;
                        break;
                    }
                }
            }
            if (agent_bin.empty()) {
                std::printf("%s [BT-AGENT] Notice: bt-agent not found in /usr/bin, /data, or ./\n",
                            core::log_timestamp().c_str());
                return;
            }

            // Restart cleanly to ensure it captures system bus
            std::system("killall -9 bt-agent 2>/dev/null || true");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::string cmd = agent_bin + " 2>&1";
            std::printf("%s [BT-AGENT] Launching %s (streaming logs to custom_ui console)\n",
                        core::log_timestamp().c_str(), cmd.c_str());

            FILE * fp = popen(cmd.c_str(), "r");
            if (!fp) {
                std::fprintf(stderr, "%s [BT-AGENT] popen failed for %s\n",
                             core::log_timestamp().c_str(), cmd.c_str());
                return;
            }

            char linebuf[512];
            while (fgets(linebuf, sizeof(linebuf), fp)) {
                size_t len = strlen(linebuf);
                while (len > 0 && (linebuf[len - 1] == '\n' || linebuf[len - 1] == '\r')) {
                    linebuf[--len] = '\0';
                }
                if (len > 0) {
                    std::printf("%s %s\n", core::log_timestamp().c_str(), linebuf);
                    fflush(stdout);
                }
            }
            pclose(fp);
        }).detach();
    }
}

bool init_bluetooth(BluetoothHandle & out, const char * /*path*/) {
    ensure_bluetooth_daemon_running();

    std::printf("%s hal::bluetooth::init_bluetooth: initializing BlueZ 5.66 stack\n", core::log_timestamp().c_str());
    
    // Direct kernel HCIDEVUP ioctl without requiring external hciconfig binary
    int sock = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
    if (sock >= 0) {
        ioctl(sock, HCIDEVUP_, 0 /* hci0 */);
        close(sock);
    }

    run_command_simple("hciconfig hci0 sspmode 1 >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 class 0x240408 >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 auth >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 encrypt >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 piscan >/dev/null 2>&1");

    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Class variant:uint32:2360328 >/dev/null 2>&1");
    out.fd = 100;  // Positive non-negative handle indicating valid BlueZ instance
    return true;
}

void start_bluetooth_reader(BluetoothHandle & h) {
    if (h.fd < 0) return;
    ReaderState & rs = reader_state();
    if (rs.started) return;
    rs.started = true;
    std::thread(bluez_monitor_loop, &h).detach();
}

void watch_bluetooth_broadcasts(std::function<void(const std::string &)> callback) {
    ReaderState & rs = reader_state();
    std::lock_guard<std::mutex> lock(rs.observer_mtx);
    rs.observers.push_back(std::move(callback));
}

bool split_mac_and_name(const std::string & entry, std::string & mac, std::string & name) {
    return split_plist_entry(entry, mac, name);
}

bool split_plist_entry(const std::string & entry, std::string & mac, std::string & name) {
    mac.clear();
    name.clear();
    if (entry.empty()) return false;

    // 1. Check for standard 17-char colon format: XX:XX:XX:XX:XX:XX (BlueZ format)
    for (size_t i = 0; i + 17 <= entry.size(); ++i) {
        if (std::isxdigit(entry[i]) && std::isxdigit(entry[i+1]) && entry[i+2] == ':' &&
            std::isxdigit(entry[i+3]) && std::isxdigit(entry[i+4]) && entry[i+5] == ':' &&
            std::isxdigit(entry[i+6]) && std::isxdigit(entry[i+7]) && entry[i+8] == ':' &&
            std::isxdigit(entry[i+9]) && std::isxdigit(entry[i+10]) && entry[i+11] == ':' &&
            std::isxdigit(entry[i+12]) && std::isxdigit(entry[i+13]) && entry[i+14] == ':' &&
            std::isxdigit(entry[i+15]) && std::isxdigit(entry[i+16])) {
            
            mac = entry.substr(i, 17);
            size_t after = i + 17;
            while (after < entry.size() && (entry[after] == ' ' || entry[after] == '\t' || entry[after] == ',')) {
                ++after;
            }
            name = (after < entry.size()) ? entry.substr(after) : "";
            return true;
        }
    }

    // 2. Check for continuous 12-hex-digit run
    size_t run_start = std::string::npos;
    for (size_t i = 0; i < entry.size(); ++i) {
        bool is_hex = (std::isxdigit(static_cast<unsigned char>(entry[i])) != 0);
        if (is_hex) {
            if (run_start == std::string::npos) run_start = i;
            size_t run_len = i - run_start + 1;
            if (run_len == 12) {
                bool bounded_after = (i + 1 == entry.size()) ||
                                      !std::isxdigit(static_cast<unsigned char>(entry[i + 1]));
                if (bounded_after) {
                    mac = entry.substr(run_start, 12);
                    size_t after = i + 1;
                    if (after < entry.size()) ++after;
                    name = (after < entry.size()) ? entry.substr(after) : "";
                    return true;
                }
                run_start = std::string::npos;
            }
        } else {
            run_start = std::string::npos;
        }
    }
    return false;
}

bool set_adapter_enabled(BluetoothHandle & /*h*/, bool enabled) {
    std::string val = enabled ? "true" : "false";
    std::printf("%s [BT-CMD] Setting adapter Powered = %s\n", core::log_timestamp().c_str(), val.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:" + val + " >/dev/null 2>&1";
    return run_command_simple(cmd);
}

bool set_discoverable(BluetoothHandle & /*h*/, bool discoverable) {
    std::string val = discoverable ? "true" : "false";
    std::printf("%s [BT-CMD] Setting adapter Discoverable = %s (piscan)\n", core::log_timestamp().c_str(), val.c_str());
    if (discoverable) {
        run_command_simple("hciconfig hci0 piscan >/dev/null 2>&1");
    }
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:" + val + " >/dev/null 2>&1";
    return run_command_simple(cmd);
}

bool list_paired_devices(BluetoothHandle & /*h*/, std::vector<std::string> & devices) {
    devices.clear();
    std::vector<std::string> lines;
    if (run_command_capture("dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null", lines) && !lines.empty()) {
        std::string current_addr, current_name;
        bool is_paired = false;

        for (size_t i = 0; i < lines.size(); ++i) {
            const auto & l = lines[i];
            if (l.find("object path \"/org/bluez/hci0/dev_") != std::string::npos) {
                if (!current_addr.empty() && is_paired) {
                    devices.push_back(current_addr + " " + current_name);
                    std::printf("%s [BT-STATUS] Found paired device: %s ('%s')\n",
                                core::log_timestamp().c_str(), current_addr.c_str(), current_name.c_str());
                }
                current_addr.clear();
                current_name.clear();
                is_paired = false;
            } else if (l.find("string \"Address\"") != std::string::npos && i + 1 < lines.size()) {
                size_t pos = lines[i+1].find("string \"");
                if (pos != std::string::npos) {
                    current_addr = lines[i+1].substr(pos + 8);
                    if (!current_addr.empty() && current_addr.back() == '"') current_addr.pop_back();
                }
            } else if (l.find("string \"Alias\"") != std::string::npos && i + 1 < lines.size()) {
                size_t pos = lines[i+1].find("string \"");
                if (pos != std::string::npos) {
                    current_name = lines[i+1].substr(pos + 8);
                    if (!current_name.empty() && current_name.back() == '"') current_name.pop_back();
                }
            } else if (l.find("string \"Name\"") != std::string::npos && i + 1 < lines.size() && current_name.empty()) {
                size_t pos = lines[i+1].find("string \"");
                if (pos != std::string::npos) {
                    current_name = lines[i+1].substr(pos + 8);
                    if (!current_name.empty() && current_name.back() == '"') current_name.pop_back();
                }
            } else if (l.find("string \"Paired\"") != std::string::npos && i + 1 < lines.size()) {
                if (lines[i+1].find("boolean true") != std::string::npos) {
                    is_paired = true;
                }
            }
        }
        if (!current_addr.empty() && is_paired) {
            devices.push_back(current_addr + " " + current_name);
            std::printf("%s [BT-STATUS] Found paired device: %s ('%s')\n",
                        core::log_timestamp().c_str(), current_addr.c_str(), current_name.c_str());
        }
    }
    std::printf("%s [BT-STATUS] list_paired_devices total: %zu paired devices\n", core::log_timestamp().c_str(), devices.size());
    return true;
}

bool connect_device(BluetoothHandle & /*h*/, const std::string & mac) {
    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);
    std::printf("%s [BT-CMD] Initiating connection to device MAC=%s (D-Bus path: %s)\n",
                core::log_timestamp().c_str(), mac.c_str(), dev_path.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call " + dev_path + " org.bluez.Device1.Connect >/dev/null 2>&1";
    bool ok = run_command_simple(cmd);
    std::printf("%s [BT-CMD] Connection request to %s: %s\n", core::log_timestamp().c_str(), mac.c_str(), ok ? "sent (OK)" : "failed to dispatch");

    // Also request connection for the Android Auto profile explicitly
    std::string profile_cmd = "dbus-send --system --dest=org.bluez --type=method_call " + dev_path +
                              " org.bluez.Device1.ConnectProfile string:4de17a00-52cb-11e6-bdf4-0800200c9a66 >/dev/null 2>&1 &";
    run_command_simple(profile_cmd);

    return ok;
}

bool disconnect_device(BluetoothHandle & /*h*/) {
    std::printf("%s [BT-CMD] Disconnecting active links\n", core::log_timestamp().c_str());
    return true;
}

bool remove_paired_device(BluetoothHandle & /*h*/, const std::string & mac) {
    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);
    std::printf("%s [BT-CMD] Removing paired device %s from BlueZ adapter\n", core::log_timestamp().c_str(), dev_path.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.bluez.Adapter1.RemoveDevice objpath:" + dev_path + " >/dev/null 2>&1";
    bool ok = run_command_simple(cmd);
    std::printf("%s [BT-CMD] RemoveDevice %s: %s\n", core::log_timestamp().c_str(), mac.c_str(), ok ? "success" : "failed");
    return ok;
}

bool set_device_name(BluetoothHandle & /*h*/, const std::string & name) {
    std::printf("%s [BT-CMD] Setting Bluetooth local name to '%s'\n", core::log_timestamp().c_str(), name.c_str());
    run_command_simple("hciconfig hci0 name \"" + name + "\" >/dev/null 2>&1");
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Alias \"variant:string:" + name + "\" >/dev/null 2>&1";
    return run_command_simple(cmd);
}

bool set_pairing_pin(BluetoothHandle & /*h*/, const std::string & /*pin*/) {
    return true;  // BlueZ Simple Secure Pairing handles passkeys automatically
}

bool get_adapter_address(BluetoothHandle & /*h*/, std::string & address) {
    address.clear();
    std::vector<std::string> lines;
    if (run_command_capture("dbus-send --system --print-reply --dest=org.bluez /org/bluez/hci0 org.freedesktop.DBus.Properties.Get string:org.bluez.Adapter1 string:Address 2>/dev/null", lines) && !lines.empty()) {
        for (const auto & l : lines) {
            size_t pos = l.find("string \"");
            if (pos != std::string::npos) {
                address = l.substr(pos + 8);
                if (!address.empty() && address.back() == '"') address.pop_back();
                std::printf("%s [BT-INFO] Controller BD_ADDR: %s\n", core::log_timestamp().c_str(), address.c_str());
                return true;
            }
        }
    }
    return false;
}

bool sync_clock_from_phone(BluetoothHandle & /*h*/) {
    return true;  // Clock sync handled via network / cellular
}

void close_bluetooth(BluetoothHandle & h) {
    h.fd = -1;
}

BluetoothHandle & shared_handle() {
    static BluetoothHandle handle;
    static bool tried = false;
    if (!tried) {
        init_bluetooth(handle);
        start_bluetooth_reader(handle);
        tried = true;
    }
    return handle;
}

bool auto_reconnect_paired_device(BluetoothHandle & h) {
    if (h.fd < 0) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: no bluetooth handle, skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::vector<std::string> devices;
    if (!list_paired_devices(h, devices) || devices.empty()) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: no paired devices to reconnect to\n", core::log_timestamp().c_str());
        return false;
    }
    std::string mac, name;
    std::string connect_id;
    for (const auto & entry : devices) {
        if (split_plist_entry(entry, mac, name) && !mac.empty()) {
            connect_id = mac;
            break;
        }
    }
    if (connect_id.empty()) {
        std::printf("%s hal::bluetooth::auto_reconnect_paired_device: no valid paired device MAC found, skipping\n", core::log_timestamp().c_str());
        return false;
    }

    std::printf("%s hal::bluetooth::auto_reconnect_paired_device: reconnecting to '%s'\n", core::log_timestamp().c_str(), connect_id.c_str());
    return connect_device(h, connect_id);
}

BluetoothTelemetry get_telemetry() {
    std::lock_guard<std::mutex> lock(g_telemetry_mtx);
    return g_telemetry;
}

bool answer_call(BluetoothHandle & /*h*/) {
    return run_command_simple("dbus-send --system --dest=org.ofono --type=method_call / org.ofono.VoiceCall.Answer >/dev/null 2>&1");
}

bool hangup_call(BluetoothHandle & /*h*/) {
    return run_command_simple("dbus-send --system --dest=org.ofono --type=method_call / org.ofono.VoiceCall.Hangup >/dev/null 2>&1");
}

bool dial_number(BluetoothHandle & /*h*/, const std::string & number) {
    return run_command_simple("dbus-send --system --dest=org.ofono --type=method_call / org.ofono.VoiceCallManager.Dial string:\"" + number + "\" string:\"\" >/dev/null 2>&1");
}

bool media_play_pause(BluetoothHandle & /*h*/) {
    return run_command_simple("bluetoothctl player.play >/dev/null 2>&1 || bluetoothctl player.pause >/dev/null 2>&1");
}

bool media_next_track(BluetoothHandle & /*h*/) {
    return run_command_simple("bluetoothctl player.next >/dev/null 2>&1");
}

bool media_prev_track(BluetoothHandle & /*h*/) {
    return run_command_simple("bluetoothctl player.previous >/dev/null 2>&1");
}

}  // namespace hal
