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
#include "core/config_store.h"
#include "hal/androidauto_client.h"
#include "hal/bluez_aa_profile.h"

namespace hal {

namespace {

std::mutex g_telemetry_mtx;
BluetoothTelemetry g_telemetry;

// See has_pending_aa_connection()/start_pending_aa_connection()'s own
// header comments -- aa_profile_server_loop() stashes a connected fd
// here instead of handing it straight to androidauto-sidecar when
// AutoStartCarLink is off. Only ever one at a time (matches
// WirelessSessionManager's own "one session at a time" model) -- a
// second phone connecting while one is already pending replaces it,
// closing the older fd rather than leaking it.
std::mutex g_pendingAaMtx;
int g_pendingAaFd = -1;

std::atomic<bool> g_aaNavigatePending{false};

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

// Reused by sync_clock_from_phone() -- same GetManagedObjects parsing
// pattern as list_paired_devices()/bluez_monitor_loop() above, just
// looking for the first currently-Connected device rather than every
// paired one.
std::string get_connected_device_mac() {
    std::vector<std::string> lines;
    if (!run_command_capture("dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null", lines) || lines.empty()) {
        return "";
    }
    std::string current_addr;
    bool current_connected = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto & l = lines[i];
        if (l.find("object path \"/org/bluez/hci0/dev_") != std::string::npos) {
            if (!current_addr.empty() && current_connected) {
                return current_addr;
            }
            current_addr.clear();
            current_connected = false;
        } else if (l.find("string \"Address\"") != std::string::npos && i + 1 < lines.size()) {
            size_t pos = lines[i+1].find("string \"");
            if (pos != std::string::npos) {
                current_addr = lines[i+1].substr(pos + 8);
                if (!current_addr.empty() && current_addr.back() == '"') current_addr.pop_back();
            }
        } else if (l.find("string \"Connected\"") != std::string::npos && i + 1 < lines.size()) {
            if (lines[i+1].find("boolean true") != std::string::npos) current_connected = true;
        }
    }
    if (!current_addr.empty() && current_connected) {
        return current_addr;
    }
    return "";
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

                    // 2026-08-20: removed a Device1.ConnectProfile(AA UUID)
                    // call that used to fire here -- confirmed against
                    // the vendored BlueZ docs (device-api.txt: "The UUID
                    // provided is the remote service UUID for the
                    // profile") that ConnectProfile's UUID argument
                    // names a service the REMOTE peer (the phone)
                    // exposes, not one we expose. A phone never runs an
                    // RFCOMM server for the AA UUID -- it's the client
                    // in this handshake (matches this project's own
                    // established finding, this file's header comment,
                    // that AA pairing/connection is phone-initiated) --
                    // so this call could only ever fail
                    // (org.bluez.Error.NotAvailable), and did nothing
                    // toward getting the phone to dial our registered
                    // Profile1 server. Real hardware log (2026-08-20)
                    // showed classic BT + A2DP connecting fine while the
                    // AA RFCOMM channel never got dialed at all, even
                    // after a fresh re-pair -- this call was a red
                    // herring, not a contributing fix. The actual server
                    // side (BluezClient::register_profile() +
                    // wait_for_connection(), androidauto-sidecar) is
                    // already in place and just waits for the phone to
                    // connect in; there's no BlueZ API to force that
                    // from our side.
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

// 2026-08-20: this thread is the whole reason Bluetooth connectivity
// can now live entirely in custom_ui -- registers the Android Auto
// wireless RFCOMM profile ONCE, then loops forever waiting for
// bluetoothd to call NewConnection (a real phone dialing in), handing
// each connected fd off to androidauto-sidecar over their local socket
// (hal::sendConnectFd()). androidauto-sidecar itself now knows nothing
// about Bluetooth/D-Bus at all -- see wireless_session_manager.h's own
// header comment for the full architecture this replaces (the sidecar
// used to run its own separate BlueZ connection just for this one
// profile).
//
// Heavily logged end to end, deliberately -- this is the one thread
// responsible for every wireless AA connection attempt ever reaching a
// session, so its progress needs to be visible in the console at every
// step, not just success/failure.
void aa_profile_server_loop() {
    std::printf("%s [BT-AA-PROFILE] server thread starting\n", core::log_timestamp().c_str());

    BluezAaProfile profile;
    if (!profile.connect()) {
        std::fprintf(stderr, "%s [BT-AA-PROFILE] could not connect to system bus -- AA wireless "
                     "will never work this boot, giving up\n", core::log_timestamp().c_str());
        return;
    }
    if (!profile.register_profile()) {
        std::fprintf(stderr, "%s [BT-AA-PROFILE] could not register AA RFCOMM profile -- AA "
                     "wireless will never work this boot, giving up\n", core::log_timestamp().c_str());
        return;
    }

    std::printf("%s [BT-AA-PROFILE] AA RFCOMM profile registered -- listening for phone "
                "connections\n", core::log_timestamp().c_str());

    int consecutiveTimeouts = 0;
    while (true) {
        int fd = profile.wait_for_connection(60);
        if (fd < 0) {
            ++consecutiveTimeouts;
            // Once a minute, every minute, forever -- proves this
            // thread is still alive and still listening, not silently
            // dead, without spamming the console every single 60s tick
            // at the same volume as a real connection attempt would.
            std::printf("%s [BT-AA-PROFILE] still waiting for a phone to connect (%d min so far)\n",
                        core::log_timestamp().c_str(), consecutiveTimeouts);
            continue;
        }
        consecutiveTimeouts = 0;
        std::printf("%s [BT-AA-PROFILE] *** phone connected over AA RFCOMM *** fd=%d\n",
                    core::log_timestamp().c_str(), fd);

        // Real Bluetooth link is up and a phone confirmed AA-capable
        // right now -- the best available moment to query it for wall-
        // clock time (see sync_clock_from_phone()'s own header comment).
        // Moved here from main.cpp's AaAutoStartWatcher, whose own
        // trigger (blueware's +AAPDEV= broadcast) has been dead since
        // this project's BlueZ migration -- watch_bluetooth_broadcasts()
        // observers are registered but nothing in this file has called
        // them since blueware's AT-command reader thread was replaced by
        // BlueZ D-Bus calls, so that call site (and the clock sync
        // riding on it) never actually ran anymore. This is the real,
        // reliable trigger now.
        sync_clock_from_phone(shared_handle());

        bool auto_start = core::default_store().get_bool("AutoStartCarLink", true, "General");
        if (auto_start) {
            std::printf("%s [BT-AA-PROFILE] AutoStartCarLink is ON -- handing off to "
                        "androidauto-sidecar immediately\n", core::log_timestamp().c_str());
            bool ok = hal::sendConnectFd(fd);
            std::printf("%s [BT-AA-PROFILE] hand-off to androidauto-sidecar: %s\n",
                        core::log_timestamp().c_str(), ok ? "accepted" : "FAILED");
            if (ok) {
                g_aaNavigatePending.store(true, std::memory_order_release);
            }
        } else {
            std::printf("%s [BT-AA-PROFILE] AutoStartCarLink is OFF -- waiting at the connect "
                        "screen for the user to start Android Auto\n", core::log_timestamp().c_str());
            std::lock_guard<std::mutex> lock(g_pendingAaMtx);
            if (g_pendingAaFd >= 0) {
                std::printf("%s [BT-AA-PROFILE] replacing an earlier still-pending connection "
                            "(fd=%d, never started) with this one\n",
                            core::log_timestamp().c_str(), g_pendingAaFd);
                close(g_pendingAaFd);
            }
            g_pendingAaFd = fd;
        }
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

    // Registers the AA RFCOMM profile and listens for phone connections
    // for the rest of this process's lifetime -- see
    // aa_profile_server_loop()'s own comment. Started here (once,
    // guarded the same way as the bt-agent thread above) rather than
    // from init_bluetooth()/main.cpp directly, so it comes up
    // automatically as soon as BlueZ itself is confirmed ready,
    // regardless of which call path first reached
    // ensure_bluetooth_daemon_running().
    static std::atomic<bool> s_aa_profile_thread_started{false};
    if (!s_aa_profile_thread_started.exchange(true)) {
        std::thread(aa_profile_server_loop).detach();
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
    // 2026-08-20: was 0x240408 (major=Audio/Video, minor=2 "Hands-free
    // device") -- see bluez-bringup.sh's own comment on this same
    // constant for the full reasoning. 0x240420 keeps the same major/
    // service class bits but sets minor=8 ("Car audio"), matching real-
    // world Android-Auto-wireless dongle projects' CoD so Android's
    // car-detection heuristic offers wireless AA setup instead of
    // treating this as a plain headset. A2DP/HFP don't care about minor
    // class, which is why audio kept working with the old value while
    // AA's RFCOMM profile never got dialed. Unconfirmed on real
    // hardware yet.
    run_command_simple("hciconfig hci0 class 0x240420 >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 auth >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 encrypt >/dev/null 2>&1");
    run_command_simple("hciconfig hci0 piscan >/dev/null 2>&1");

    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Pairable variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Discoverable variant:boolean:true >/dev/null 2>&1");
    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Class variant:uint32:2360352 >/dev/null 2>&1");
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

    // 2026-08-20: removed a second Device1.ConnectProfile(AA UUID) call
    // here for the same reason as bluez_monitor_loop's own auto-fire
    // (removed above, see its comment) -- ConnectProfile's UUID names a
    // service the REMOTE device exposes (confirmed against the vendored
    // BlueZ docs, device-api.txt), and a phone never runs an RFCOMM
    // server for the AA UUID. This call could never succeed and wasn't
    // moving the actual handshake forward; Device1.Connect() above is
    // the real, useful part of this function (brings up the baseband
    // ACL link so BlueZ can serve our already-registered AA Profile1 to
    // the phone once it decides to dial in).

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

// 2026-08-20: real implementation, replacing a stub that just returned
// true unconditionally. Every AT-command-era clock-sync avenue
// (AT+CCLK, HFPTIME, BLE CTS, PBAP call history, AA WiFi-SoftAP SNTP,
// AA SensorChannel GPS) is a confirmed dead end for this project (see
// memory: project_clock_sync_avenues_exhausted.md) -- but that
// investigation's one BlueZ-shaped lead was closed based on
// `blueware`'s own limits (no raw-HCI passthrough mode), not on the
// real native BlueZ stack this session brought up actually being
// tried. With a genuine independent hci0/bluetoothd now confirmed
// working, org.bluez.Network1.Connect("nap") (client role -- connect
// OUT to the phone's own Bluetooth-tethering hotspot, distinct from
// this device's own NetworkServer1/NAP role that bluetoothd's
// "network" plugin already advertises for incoming connections) is a
// real, previously-untried path: if the phone has Bluetooth tethering
// enabled, this hands back a bnep0-style interface with real internet
// behind it, enough for one NTP query.
//
// Entirely best-effort and bounded -- most real-world runs will have
// tethering off and simply fail step 1, matching every other optional-
// path HAL convention in this codebase. System-wide via busybox's own
// ntpd (not a hand-rolled NTP client or a raw settimeofday() call) --
// per explicit user direction this session, overriding the earlier
// removed boot-time settimeofday() (see memory:
// feedback_no_boot_time_clock_override.md); this call site (a real
// Bluetooth link already up and a real phone confirmed nearby, not an
// unconditional boot-time guess) is the targeted case that revert was
// about avoiding.
//
// bnep0 is disconnected again afterward -- this is a one-shot clock
// query, not meant to become a persistent second internet route
// alongside this device's own WiFi AP.
bool sync_clock_from_phone(BluetoothHandle & h) {
    if (h.fd < 0) {
        return false;
    }
    std::string mac = get_connected_device_mac();
    if (mac.empty()) {
        std::printf("%s hal::bluetooth::sync_clock_from_phone: no currently-connected device, "
                    "skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);

    std::vector<std::string> connectLines;
    std::string connectCmd = "dbus-send --system --print-reply --dest=org.bluez " + dev_path +
                              " org.bluez.Network1.Connect string:nap 2>&1";
    if (!run_command_capture(connectCmd, connectLines) || connectLines.empty()) {
        std::printf("%s hal::bluetooth::sync_clock_from_phone: Network1.Connect(nap) failed for "
                    "%s (phone likely has Bluetooth tethering off) -- skipping\n",
                    core::log_timestamp().c_str(), mac.c_str());
        return false;
    }

    std::string iface;
    for (const auto & l : connectLines) {
        size_t pos = l.find("string \"");
        if (pos != std::string::npos) {
            iface = l.substr(pos + 8);
            if (!iface.empty() && iface.back() == '"') iface.pop_back();
            break;
        }
    }
    if (iface.empty()) {
        std::printf("%s hal::bluetooth::sync_clock_from_phone: Network1.Connect(nap) reply had no "
                    "interface name -- skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::printf("%s hal::bluetooth::sync_clock_from_phone: PAN link up on %s, requesting DHCP "
                "lease...\n", core::log_timestamp().c_str(), iface.c_str());

    // -n: exit rather than daemonize if no lease; -q: quit once a lease
    // is obtained; -T/-t: bound total wait to a few seconds each try --
    // this must never hang the caller if the phone's own DHCP server
    // (part of its tethering stack) doesn't answer.
    std::string dhcpCmd = "udhcpc -i " + iface + " -n -q -T 3 -t 3 >/dev/null 2>&1";
    bool haveLease = (std::system(dhcpCmd.c_str()) == 0);

    bool synced = false;
    if (haveLease) {
        std::printf("%s hal::bluetooth::sync_clock_from_phone: %s has an IP, querying NTP...\n",
                    core::log_timestamp().c_str(), iface.c_str());
        // busybox ntpd's own one-shot mode: queries, sets CLOCK_REALTIME
        // itself, and exits -- no hand-rolled NTP parsing needed.
        synced = (std::system("ntpd -n -q -p pool.ntp.org >/dev/null 2>&1") == 0);
        std::printf("%s hal::bluetooth::sync_clock_from_phone: system clock sync %s\n",
                    core::log_timestamp().c_str(), synced ? "succeeded" : "failed (ntpd query)");
    } else {
        std::printf("%s hal::bluetooth::sync_clock_from_phone: no DHCP lease on %s -- skipping "
                    "NTP query\n", core::log_timestamp().c_str(), iface.c_str());
    }

    run_command_simple("dbus-send --system --dest=org.bluez --type=method_call " + dev_path +
                        " org.bluez.Network1.Disconnect >/dev/null 2>&1");
    return synced;
}

bool has_pending_aa_connection() {
    std::lock_guard<std::mutex> lock(g_pendingAaMtx);
    return g_pendingAaFd >= 0;
}

bool start_pending_aa_connection() {
    int fd;
    {
        std::lock_guard<std::mutex> lock(g_pendingAaMtx);
        if (g_pendingAaFd < 0) {
            return false;
        }
        fd = g_pendingAaFd;
        g_pendingAaFd = -1;
    }
    std::printf("%s [BT-AA-PROFILE] user started Android Auto -- handing off pending fd=%d to "
                "androidauto-sidecar\n", core::log_timestamp().c_str(), fd);
    bool ok = hal::sendConnectFd(fd);
    std::printf("%s [BT-AA-PROFILE] hand-off to androidauto-sidecar: %s\n",
                core::log_timestamp().c_str(), ok ? "accepted" : "FAILED");
    return ok;
}

bool consume_aa_navigate_request() {
    return g_aaNavigatePending.exchange(false, std::memory_order_acq_rel);
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
