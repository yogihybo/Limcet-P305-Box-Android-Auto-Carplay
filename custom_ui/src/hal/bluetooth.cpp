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

#include <dbus/dbus.h>

#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_  1
#define HCIDEVUP_     _IOW('H', 201, int)

#include "core/hal_config.h"
#include "core/log_timing.h"
#include "core/config_store.h"
#include "core/sized_thread.h"
#include "hal/androidauto_client.h"
#include "hal/ble_cts.h"
#include "hal/bluez_aa_profile.h"

namespace hal {

namespace {

std::mutex g_telemetry_mtx;
BluetoothTelemetry g_telemetry;

// Matches hal/bluez_aa_profile.cpp's/hal/ble_cts.cpp's own kBusAddress --
// this device's real dbus-daemon (see tools/bluetoothd-test/README.md)
// resolves its default listen address to this doubled `run/run` path.
constexpr const char * kBluezMonitorBusAddress = "unix:path=/var/run/dbus/system_bus_socket";

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

}  // namespace

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

namespace {

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

// Walks a GetManagedObjects reply (a{oa{sa{sv}}}) via real DBusMessageIter
// traversal -- shared by get_connected_device_mac() and the signal-driven
// monitor below. Replaces the old per-call `popen("dbus-send ...")` +
// line-based text parsing (see BluezMonitorState's own comment for why
// that was replaced) with one already-open connection's synchronous
// method call, matching the pattern hal/ble_cts.cpp's
// find_cts_characteristic() already established in this codebase.
struct DeviceSnapshot {
    std::string address;
    std::string name;
    bool connected = false;
    bool paired = false;
    int rssi = -1;
};

// 2026-09-05: real hardware bug found via code review -- refresh_bluez_telemetry()
// only ever queried org.bluez.Device1 (connected/name/rssi), never
// org.bluez.MediaPlayer1 (the Track dict + Status string BlueZ exposes
// for an active AVRCP session), so BluetoothTelemetry::track_title/
// track_artist/play_status were declared in the header but NEVER
// actually written anywhere in this file -- the Home Dashboard's media
// card showed "Not Playing" permanently, even with Bluetooth audio
// actively playing through the car speakers. A MediaPlayer1 object is
// its own top-level entry in the SAME GetManagedObjects response
// fetch_managed_devices() already walks (nested under the owning
// device's own path, e.g. .../dev_XX_.../player0, but a sibling ENTRY
// in the outer loop, not nested inside Device1's own property dict) --
// extracted here in the same pass rather than a second D-Bus round-trip.
struct PlayerSnapshot {
    std::string title;
    std::string artist;
    std::string album;
    int status = 0;  // 0=stopped, 1=playing, 2=paused -- matches BluetoothTelemetry::play_status
    bool found = false;
};

bool fetch_managed_devices(DBusConnection * conn, std::vector<DeviceSnapshot> & out,
                            PlayerSnapshot * player_out = nullptr) {
    DBusMessage * msg = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) {
            if (err.name && std::strcmp(err.name, "org.freedesktop.DBus.Error.ServiceUnknown") != 0) {
                std::fprintf(stderr, "%s [BT] GetManagedObjects failed: %s\n",
                             core::log_timestamp().c_str(), err.message);
            }
            dbus_error_free(&err);
        }
        return false;
    }

    DBusMessageIter iter, objects;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&objects, &entry);
        const char * path = nullptr;
        dbus_message_iter_get_basic(&entry, &path);
        dbus_message_iter_next(&entry);

        std::string pathStr(path ? path : "");
        // Real BlueZ device object paths are /org/bluez/hci0/dev_XX_XX...
        // -- adapter/root/other interfaces at shorter paths are skipped,
        // matching the old code's own "object path .../dev_" line match.
        bool isDevice = pathStr.rfind("/org/bluez/hci0/dev_", 0) == 0;

        DBusMessageIter ifaces;
        dbus_message_iter_recurse(&entry, &ifaces);
        DeviceSnapshot snap;
        bool sawDevice1 = false;
        while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ifaceEntry;
            dbus_message_iter_recurse(&ifaces, &ifaceEntry);
            const char * ifaceName = nullptr;
            dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);
            dbus_message_iter_next(&ifaceEntry);

            if (isDevice && ifaceName && std::strcmp(ifaceName, "org.bluez.Device1") == 0) {
                sawDevice1 = true;
                DBusMessageIter props;
                dbus_message_iter_recurse(&ifaceEntry, &props);
                while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter propEntry;
                    dbus_message_iter_recurse(&props, &propEntry);
                    const char * propName = nullptr;
                    dbus_message_iter_get_basic(&propEntry, &propName);
                    dbus_message_iter_next(&propEntry);
                    if (propName && dbus_message_iter_get_arg_type(&propEntry) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter variant;
                        dbus_message_iter_recurse(&propEntry, &variant);
                        int vtype = dbus_message_iter_get_arg_type(&variant);
                        if (std::strcmp(propName, "Address") == 0 && vtype == DBUS_TYPE_STRING) {
                            const char * v = nullptr;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.address = v ? v : "";
                        } else if (std::strcmp(propName, "Name") == 0 && vtype == DBUS_TYPE_STRING) {
                            const char * v = nullptr;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.name = v ? v : "";
                        } else if (std::strcmp(propName, "Alias") == 0 && vtype == DBUS_TYPE_STRING &&
                                   snap.name.empty()) {
                            const char * v = nullptr;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.name = v ? v : "";
                        } else if (std::strcmp(propName, "Connected") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t v = FALSE;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.connected = v;
                        } else if (std::strcmp(propName, "Paired") == 0 && vtype == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t v = FALSE;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.paired = v;
                        } else if (std::strcmp(propName, "RSSI") == 0 &&
                                   (vtype == DBUS_TYPE_INT16 || vtype == DBUS_TYPE_INT32)) {
                            dbus_int32_t v = 0;
                            dbus_message_iter_get_basic(&variant, &v);
                            snap.rssi = v;
                        }
                    }
                    dbus_message_iter_next(&props);
                }
            } else if (player_out && ifaceName && std::strcmp(ifaceName, "org.bluez.MediaPlayer1") == 0) {
                DBusMessageIter props;
                dbus_message_iter_recurse(&ifaceEntry, &props);
                while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter propEntry;
                    dbus_message_iter_recurse(&props, &propEntry);
                    const char * propName = nullptr;
                    dbus_message_iter_get_basic(&propEntry, &propName);
                    dbus_message_iter_next(&propEntry);
                    if (propName && dbus_message_iter_get_arg_type(&propEntry) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter variant;
                        dbus_message_iter_recurse(&propEntry, &variant);
                        int vtype = dbus_message_iter_get_arg_type(&variant);
                        if (std::strcmp(propName, "Status") == 0 && vtype == DBUS_TYPE_STRING) {
                            const char * v = nullptr;
                            dbus_message_iter_get_basic(&variant, &v);
                            std::string status = v ? v : "";
                            player_out->status = (status == "playing") ? 1
                                                : (status == "paused") ? 2
                                                : 0;
                            player_out->found = true;
                        } else if (std::strcmp(propName, "Track") == 0 && vtype == DBUS_TYPE_ARRAY) {
                            // Track is itself a{sv} -- one more level of
                            // dict-entry/variant nesting than the flat
                            // Device1 properties above.
                            DBusMessageIter track;
                            dbus_message_iter_recurse(&variant, &track);
                            while (dbus_message_iter_get_arg_type(&track) == DBUS_TYPE_DICT_ENTRY) {
                                DBusMessageIter trackEntry;
                                dbus_message_iter_recurse(&track, &trackEntry);
                                const char * trackKey = nullptr;
                                dbus_message_iter_get_basic(&trackEntry, &trackKey);
                                dbus_message_iter_next(&trackEntry);
                                if (trackKey && dbus_message_iter_get_arg_type(&trackEntry) == DBUS_TYPE_VARIANT) {
                                    DBusMessageIter trackVariant;
                                    dbus_message_iter_recurse(&trackEntry, &trackVariant);
                                    if (dbus_message_iter_get_arg_type(&trackVariant) == DBUS_TYPE_STRING) {
                                        const char * v = nullptr;
                                        dbus_message_iter_get_basic(&trackVariant, &v);
                                        if (std::strcmp(trackKey, "Title") == 0) {
                                            player_out->title = v ? v : "";
                                            player_out->found = true;
                                        } else if (std::strcmp(trackKey, "Artist") == 0) {
                                            player_out->artist = v ? v : "";
                                            player_out->found = true;
                                        } else if (std::strcmp(trackKey, "Album") == 0) {
                                            player_out->album = v ? v : "";
                                        }
                                    }
                                }
                                dbus_message_iter_next(&track);
                            }
                        }
                    }
                    dbus_message_iter_next(&props);
                }
            }
            dbus_message_iter_next(&ifaces);
        }
        if (isDevice && sawDevice1 && !snap.address.empty()) {
            out.push_back(std::move(snap));
        }
        dbus_message_iter_next(&objects);
    }
    dbus_message_unref(reply);
    return true;
}

// fetch_managed_devices helper is defined above


// 2026-08-21: replaces a `while(true) { popen("dbus-send ...", 1.5s
// sleep }` loop that ran for the ENTIRE process lifetime -- confirmed
// real hardware finding: that was a full fork()+exec("dbus-send") every
// 1.5 seconds, each one opening a brand-new D-Bus system bus connection
// from scratch, dumping every BlueZ object/interface/property as text,
// which this code then re-parsed line-by-line -- ~40 process spawns/
// minute, continuously, for as long as the device is on. On a 173MB-RAM
// device with no swap, traced as a real contributor to the sustained
// page-cache/inode-churn growth behind an eventual thrashing-induced AA
// session drop (ECONNRESET) after several minutes of runtime -- see
// project memory/session history for the fuller trace.
//
// Now: ONE persistent libdbus connection for the process's entire
// lifetime (matching hal/bluez_aa_profile.cpp's/hal/ble_cts.cpp's own
// established pattern), subscribed to the real BlueZ signals
// (PropertiesChanged under every /org/bluez/... object, and
// InterfacesAdded/Removed on the root ObjectManager) instead of a fixed
// timer. dbus_connection_read_write_dispatch() blocks (bounded by a
// timeout so this can still notice h->fd going invalid and exit) until
// something ACTUALLY happens on the bus -- near-zero idle CPU/process
// churn instead of continuous polling, and telemetry updates are
// event-driven rather than up-to-1.5s-stale.
struct BluezMonitorState {
    bool last_connected = false;
    std::string last_connected_mac;
    std::string last_connected_name;
    size_t last_device_count = 0;
};

void refresh_bluez_telemetry(DBusConnection * conn, BluezMonitorState & state) {
    std::vector<DeviceSnapshot> devices;
    PlayerSnapshot player;
    if (!fetch_managed_devices(conn, devices, &player)) {
        std::lock_guard<std::mutex> lock(g_telemetry_mtx);
        if (state.last_connected) {
            std::printf("%s [BT] Lost connection to BlueZ daemon\n", core::log_timestamp().c_str());
            state.last_connected = false;
        }
        g_telemetry.connected = false;
        return;
    }

    std::lock_guard<std::mutex> lock(g_telemetry_mtx);
    bool connected = false;
    std::string connected_mac, connected_name;
    int rssi = -1;
    for (const auto & d : devices) {
        if (d.connected) {
            connected = true;
            connected_mac = d.address;
            connected_name = d.name;
            rssi = d.rssi;
            break;
        }
    }

    if (connected != state.last_connected || connected_mac != state.last_connected_mac) {
        if (connected) {
            std::printf("%s [BT] *** Device Connected ***: %s ('%s') RSSI=%d dBm\n",
                        core::log_timestamp().c_str(), connected_mac.c_str(),
                        connected_name.c_str(), rssi);
            // 2026-08-20: no Device1.ConnectProfile(AA UUID) call here --
            // see git history for this line's own prior comment on why
            // that call was removed (AA pairing is phone-initiated; the
            // UUID argument would have named a service the REMOTE peer
            // exposes, not one we expose, so it could only ever fail).
        } else {
            std::printf("%s [BT] *** Device Disconnected ***: (was %s '%s')\n",
                        core::log_timestamp().c_str(), state.last_connected_mac.c_str(),
                        state.last_connected_name.c_str());
        }
        state.last_connected = connected;
        state.last_connected_mac = connected_mac;
        state.last_connected_name = connected_name;
    }

    if (devices.size() != state.last_device_count) {
        std::printf("%s [BT] Total known devices in BlueZ database: %zu\n",
                    core::log_timestamp().c_str(), devices.size());
        state.last_device_count = devices.size();
    }

    g_telemetry.connected = connected;
    if (!connected_name.empty()) g_telemetry.connected_device_name = connected_name;
    g_telemetry.signal_strength = rssi;

    // MediaPlayer1 only exists while a device has an active AVRCP media
    // session -- clear the track fields when it's gone (device
    // disconnected, or just not playing anything) rather than showing
    // stale metadata from whatever last played.
    if (player.found) {
        g_telemetry.track_title = player.title;
        g_telemetry.track_artist = player.artist;
        g_telemetry.track_album = player.album;
        g_telemetry.play_status = player.status;
    } else {
        g_telemetry.track_title.clear();
        g_telemetry.track_artist.clear();
        g_telemetry.track_album.clear();
        g_telemetry.play_status = 0;
    }
}

// 2026-08-21: only ever sets a flag here, deliberately -- does NOT call
// refresh_bluez_telemetry() (a blocking dbus_connection_send_with_reply_
// and_block() call) directly from this filter. libdbus filter functions
// run synchronously from inside dbus_connection_dispatch()'s own
// processing of this connection's incoming queue; making a blocking
// send-and-wait call reentrantly from within that is a real, documented
// libdbus footgun (message-ordering corruption at best, a hang at
// worst) -- not the same as this codebase's other libdbus call sites
// (hal/ble_cts.cpp/hal/bluez_aa_profile.cpp), which all call
// send_with_reply_and_block() from ordinary top-level flow, never from
// inside a filter. bluez_monitor_loop()'s own while loop checks this
// flag and does the actual refresh AFTER dispatch returns instead,
// where a blocking call back onto the same connection is safe.
std::atomic<bool> g_bluezTelemetryDirty{false};

DBusHandlerResult bluez_signal_filter(DBusConnection *, DBusMessage * msg, void *) {
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged") ||
        dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded") ||
        dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
        g_bluezTelemetryDirty.store(true, std::memory_order_relaxed);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void bluez_monitor_loop(BluetoothHandle * h) {
    std::printf("%s [BT] BlueZ status monitor thread started\n", core::log_timestamp().c_str());

    DBusError err;
    dbus_error_init(&err);
    DBusConnection * conn = dbus_connection_open_private(kBluezMonitorBusAddress, &err);
    if (!conn) {
        std::fprintf(stderr, "%s [BT] BlueZ monitor could not open D-Bus connection: %s\n",
                     core::log_timestamp().c_str(), err.message ? err.message : "(no message)");
        dbus_error_free(&err);
        return;
    }
    dbus_error_init(&err);
    if (!dbus_bus_register(conn, &err)) {
        std::fprintf(stderr, "%s [BT] BlueZ monitor dbus_bus_register failed: %s\n",
                     core::log_timestamp().c_str(), err.message ? err.message : "(no message)");
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }

    // path_namespace matches every object under /org/bluez (adapter,
    // every device, every GATT service/characteristic) -- a real
    // D-Bus match-rule feature (spec since dbus 1.5), not a prefix hack.
    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path_namespace='/org/bluez'", &err);
    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded',path='/'", &err);
    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesRemoved',path='/'", &err);
    dbus_connection_flush(conn);

    BluezMonitorState state;
    dbus_connection_add_filter(conn, bluez_signal_filter, nullptr, nullptr);

    // Seed initial telemetry once -- signals only fire on FUTURE
    // changes, they say nothing about the state at the moment this
    // thread started (e.g. a device already connected before this
    // thread ran its first dispatch).
    refresh_bluez_telemetry(conn, state);

    while (h->fd >= 0) {
        // Blocks (bounded) until real bus traffic arrives or the
        // timeout elapses -- the bounded timeout exists only so this
        // loop can still notice h->fd going invalid and exit cleanly,
        // not as a polling interval (the actual refresh below, driven
        // by g_bluezTelemetryDirty, is what responds to real events;
        // this call doing nothing for the whole timeout is the normal,
        // expected, near-zero-CPU idle case).
        dbus_connection_read_write_dispatch(conn, 5000);
        if (g_bluezTelemetryDirty.exchange(false, std::memory_order_relaxed)) {
            refresh_bluez_telemetry(conn, state);
        }
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
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
    std::printf("%s [BT] server thread starting\n", core::log_timestamp().c_str());

    BluezAaProfile profile;
    bool connected = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (profile.connect()) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!connected) {
        std::fprintf(stderr, "%s [BT] could not connect to system bus after 30s -- giving up\n",
                     core::log_timestamp().c_str());
        return;
    }

    bool registered = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (profile.register_profile()) {
            registered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!registered) {
        std::fprintf(stderr, "%s [BT] could not register AA RFCOMM profile after 30s -- giving up\n",
                     core::log_timestamp().c_str());
        return;
    }

    std::printf("%s [BT] AA RFCOMM profile registered -- listening for phone "
                "connections\n", core::log_timestamp().c_str());

    int consecutiveTimeouts = 0;
    // 2026-08-21: tracks whether this loop has EVER successfully handed
    // a connected fd off to androidauto-sidecar -- purely for the log
    // wording below, not used for any real logic. This loop only ever
    // waits for the NEXT org.bluez.Profile1.NewConnection call; it has
    // no visibility into whether a session it already handed off is
    // still alive (that's androidauto-sidecar's own state, a separate
    // process). Without this, "still waiting for a phone to connect"
    // kept printing every 60s even with a real, currently-active AA
    // session running -- a real (if once-off) connection had already
    // happened, this loop was correctly just idle waiting for a SECOND
    // one that was never coming, but the wording read as "nothing is
    // connected", which is misleading during an active session.
    bool everHandedOff = false;
    while (true) {
        int fd = profile.wait_for_connection(60);
        if (fd < 0) {
            ++consecutiveTimeouts;
            // Once a minute, every minute, forever -- proves this
            // thread is still alive and still listening, not silently
            // dead, without spamming the console every single 60s tick
            // at the same volume as a real connection attempt would.
            if (everHandedOff) {
                std::printf("%s [BT] no NEW phone connection in the last %d min "
                            "(this only tracks additional RFCOMM connections -- an existing "
                            "AA session, if any, is unaffected)\n",
                            core::log_timestamp().c_str(), consecutiveTimeouts);
            } else {
                std::printf("%s [BT] still waiting for a phone to connect (%d min so far)\n",
                            core::log_timestamp().c_str(), consecutiveTimeouts);
            }
            continue;
        }
        consecutiveTimeouts = 0;
        std::printf("%s [BT] *** phone connected over AA RFCOMM *** fd=%d\n",
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
        // Spawn clock sync in the background so it never delays AA hand-off
        core::SizedThread(core::kDefaultThreadStackSize, []() {
            sync_clock_from_phone(shared_handle());
        }).detach();

        bool auto_start = core::default_store().get_bool("AutoStartCarLink", true, "General");
        if (auto_start) {
            std::printf("%s [BT] AutoStartCarLink is ON -- handing off to "
                        "androidauto-sidecar immediately\n", core::log_timestamp().c_str());
            bool ok = hal::sendConnectFd(fd);
            std::printf("%s [BT] hand-off to androidauto-sidecar: %s\n",
                        core::log_timestamp().c_str(), ok ? "accepted" : "FAILED");
            if (ok) {
                g_aaNavigatePending.store(true, std::memory_order_release);
                everHandedOff = true;
            }
        } else {
            std::printf("%s [BT] AutoStartCarLink is OFF -- waiting at the connect "
                        "screen for the user to start Android Auto\n", core::log_timestamp().c_str());
            std::lock_guard<std::mutex> lock(g_pendingAaMtx);
            if (g_pendingAaFd >= 0) {
                std::printf("%s [BT] replacing an earlier still-pending connection "
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
    // 2026-08-23: real hw bug -- this used to `return` here once BlueZ
    // was found already running, which made sense back when this
    // function was the ONLY place that ever started dbus-daemon/
    // rtk_hciattach/bluetoothd. It no longer is: rcS now starts all
    // three unconditionally at boot, so is_bluez_active() is true on
    // every real run, and the early `return` was silently skipping
    // the bt-agent + AA RFCOMM profile bootstrap further down in this
    // same function -- hardware-confirmed via a full boot log with
    // ZERO [BT-AGENT]/[BT-AA-PROFILE] output despite the phone
    // connecting and disconnecting twice. Both of those have their
    // own std::atomic-guarded start-once logic already, so it's safe
    // to just skip the daemon-bringup block below instead of the
    // whole function.
    bool blueZAlreadyActive = is_bluez_active();
    if (blueZAlreadyActive) {
        std::printf("%s [BT] BlueZ (bluetoothd) already running\n", core::log_timestamp().c_str());
    }

  if (!blueZAlreadyActive) {
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
            std::printf("%s [BT] Invoking %s start\n", core::log_timestamp().c_str(), s);
            std::string cmd = std::string(s) + " start >/dev/null 2>&1";
            std::system(cmd.c_str());
            break;
        }
    }

    // Check if rtk_hciattach is already running / attaching hci0
    if (std::system("pidof rtk_hciattach >/dev/null 2>&1") != 0) {
        std::printf("%s [BT] Starting rtk_hciattach over /dev/ttyHS1\n", core::log_timestamp().c_str());
        std::system("rtk_hciattach -n -s 115200 /dev/ttyHS1 rtk_h5 >/dev/null 2>&1 &");
        for (int i = 0; i < 30; ++i) {
            struct stat st {};
            if (stat("/sys/class/bluetooth/hci0", &st) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (std::system("pidof dbus-daemon >/dev/null 2>&1") != 0) {
        std::system("mkdir -p /var/run/dbus && dbus-daemon --system --fork >/dev/null 2>&1");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (std::system("pidof bluetoothd >/dev/null 2>&1") != 0) {
        std::printf("%s [BT] Starting bluetoothd\n", core::log_timestamp().c_str());
        std::system("bluetoothd -n >/dev/null 2>&1 &");
        for (int i = 0; i < 20; ++i) {
            if (std::system("pidof bluetoothd >/dev/null 2>&1") == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
  }  // !blueZAlreadyActive

    // Ensure bt-agent is running and stream its output directly to custom_ui console
    static std::atomic<bool> s_agent_thread_started{false};
    if (!s_agent_thread_started.exchange(true)) {
        core::SizedThread(core::kDefaultThreadStackSize, []() {
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
                std::printf("%s [BT:AGENT] Notice: bt-agent not found in /usr/bin, /data, or ./\n",
                            core::log_timestamp().c_str());
                return;
            }

            // Wait for BlueZ daemon to be up and responsive
            for (int i = 0; i < 60; ++i) {
                if (is_bluez_active()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Restart cleanly to ensure it captures system bus
            std::system("killall -9 bt-agent 2>/dev/null || true");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::string cmd = agent_bin + " 2>&1";
            std::printf("%s [BT:AGENT] Launching %s (streaming logs to custom_ui console)\n",
                        core::log_timestamp().c_str(), cmd.c_str());

            FILE * fp = popen(cmd.c_str(), "r");
            if (!fp) {
                std::fprintf(stderr, "%s [BT:AGENT] ERROR: popen failed for %s\n",
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

    static std::atomic<bool> s_aa_profile_thread_started{false};
    if (!s_aa_profile_thread_started.exchange(true)) {
        core::SizedThread(core::kDefaultThreadStackSize, aa_profile_server_loop).detach();
    }
}

bool init_bluetooth(BluetoothHandle & out, const char * /*path*/) {
    ensure_bluetooth_daemon_running();

    std::printf("%s [BT] Initializing BlueZ 5.66 stack\n", core::log_timestamp().c_str());
    
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
    core::SizedThread(core::kDefaultThreadStackSize, bluez_monitor_loop, &h).detach();
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
    std::printf("%s [BT] Setting adapter Powered = %s\n", core::log_timestamp().c_str(), val.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 string:Powered variant:boolean:" + val + " >/dev/null 2>&1";
    return run_command_simple(cmd);
}

bool set_discoverable(BluetoothHandle & /*h*/, bool discoverable) {
    std::string val = discoverable ? "true" : "false";
    std::printf("%s [BT] Setting adapter Discoverable = %s (piscan)\n", core::log_timestamp().c_str(), val.c_str());
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
                    std::printf("%s [BT] Found paired device: %s ('%s')\n",
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
            std::printf("%s [BT] Found paired device: %s ('%s')\n",
                        core::log_timestamp().c_str(), current_addr.c_str(), current_name.c_str());
        }
    }
    std::printf("%s [BT] list_paired_devices total: %zu paired devices\n", core::log_timestamp().c_str(), devices.size());
    return true;
}

bool connect_device(BluetoothHandle & /*h*/, const std::string & mac) {
    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);
    std::printf("%s [BT] Initiating connection to device MAC=%s (D-Bus path: %s)\n",
                core::log_timestamp().c_str(), mac.c_str(), dev_path.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call " + dev_path + " org.bluez.Device1.Connect >/dev/null 2>&1";
    bool ok = run_command_simple(cmd);
    std::printf("%s [BT] Connection request to %s: %s\n", core::log_timestamp().c_str(), mac.c_str(), ok ? "sent (OK)" : "failed to dispatch");

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
    std::printf("%s [BT] Disconnecting active links\n", core::log_timestamp().c_str());
    return true;
}

bool remove_paired_device(BluetoothHandle & /*h*/, const std::string & mac) {
    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);
    std::printf("%s [BT] Removing paired device %s from BlueZ adapter\n", core::log_timestamp().c_str(), dev_path.c_str());
    std::string cmd = "dbus-send --system --dest=org.bluez --type=method_call /org/bluez/hci0 org.bluez.Adapter1.RemoveDevice objpath:" + dev_path + " >/dev/null 2>&1";
    bool ok = run_command_simple(cmd);
    std::printf("%s [BT] RemoveDevice %s: %s\n", core::log_timestamp().c_str(), mac.c_str(), ok ? "success" : "failed");
    return ok;
}

bool set_device_name(BluetoothHandle & /*h*/, const std::string & name) {
    std::printf("%s [BT] Setting Bluetooth local name to '%s'\n", core::log_timestamp().c_str(), name.c_str());
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
                std::printf("%s [BT] Controller BD_ADDR: %s\n", core::log_timestamp().c_str(), address.c_str());
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
        std::printf("%s [BT] No currently-connected device, skipping clock sync\n", core::log_timestamp().c_str());
        return false;
    }

    if (sync_clock_via_ble_cts(mac)) {
        std::printf("%s [BT] BLE CTS succeeded, skipping PAN/NTP path\n", core::log_timestamp().c_str());
        return true;
    }
    std::printf("%s [BT] BLE CTS not available -- trying Bluetooth PAN/NTP (1 attempt)\n", core::log_timestamp().c_str());

    std::string dev_path = "/org/bluez/hci0/" + mac_to_dbus_path(mac);

    std::vector<std::string> connectLines;
    std::string connectCmd = "dbus-send --system --print-reply --dest=org.bluez " + dev_path +
                              " org.bluez.Network1.Connect string:nap 2>&1";
    if (!run_command_capture(connectCmd, connectLines) || connectLines.empty()) {
        std::printf("%s [BT] Network1.Connect(nap) failed for %s (Bluetooth tethering inactive)\n",
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
        std::printf("%s [BT] Network1.Connect(nap) reply had no interface name -- skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::printf("%s [BT] PAN link up on %s, requesting DHCP lease...\n", core::log_timestamp().c_str(), iface.c_str());

    // 2026-08-21: real hardware showed the very next udhcpc call failing
    // in ~40ms -- WAY faster than udhcpc's own -T 3 -t 3 bounded retry
    // could ever produce for a real "no DHCP server answered" timeout,
    // which pointed at a race instead: Network1.Connect's D-Bus reply
    // (the iface name above) can come back before the kernel BNEP
    // interface it names has actually finished coming up, so udhcpc's
    // own immediate "no such device"-class failure looked identical to
    // a real DHCP timeout in the log. Bounded poll for the interface to
    // actually exist first (/sys/class/net/<iface>, same source ifconfig/
    // ip itself reads) -- up to 2s, generous for a local kernel netdev
    // to appear but nowhere near udhcpc's own budget, so this can't turn
    // a genuine "phone has no NAP/tethering active" case into a long
    // hang. Fails gracefully either way: if the interface never shows
    // up, skip DHCP/NTP entirely and return false, same as every other
    // failure branch in this function -- BLE CTS (tried before this
    // function's PAN path even starts) already covers the no-tethering
    // case without ever reaching here.
    bool ifaceReady = false;
    std::string ifaceSysPath = "/sys/class/net/" + iface;
    for (int attempt = 0; attempt < 3; ++attempt) {
        struct stat st{};
        if (::stat(ifaceSysPath.c_str(), &st) == 0) {
            ifaceReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ifaceReady) {
        std::printf("%s [BT] %s never appeared -- skipping DHCP/NTP\n", core::log_timestamp().c_str(), iface.c_str());
        run_command_simple("dbus-send --system --dest=org.bluez --type=method_call " + dev_path +
                            " org.bluez.Network1.Disconnect >/dev/null 2>&1");
        return false;
    }

    std::string dhcpCmd = "udhcpc -i " + iface + " -s /etc/udhcpc.script -n -q -T 1 -t 1 >/dev/null 2>&1";
    bool haveLease = (std::system(dhcpCmd.c_str()) == 0);

    bool synced = false;
    if (haveLease) {
        std::printf("%s [BT] %s has an IP, querying NTP...\n",
                    core::log_timestamp().c_str(), iface.c_str());
        synced = (std::system("ntpd -n -q -p 216.239.35.0 >/dev/null 2>&1") == 0);
        std::printf("%s [BT] System clock sync %s\n",
                    core::log_timestamp().c_str(), synced ? "succeeded" : "failed (ntpd query)");
    } else {
        std::printf("%s [BT] No DHCP lease on %s -- skipping NTP query\n", core::log_timestamp().c_str(), iface.c_str());
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
    std::printf("%s [BT] user started Android Auto -- handing off pending fd=%d to "
                "androidauto-sidecar\n", core::log_timestamp().c_str(), fd);
    bool ok = hal::sendConnectFd(fd);
    std::printf("%s [BT] hand-off to androidauto-sidecar: %s\n",
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
        std::printf("%s [BT] auto_reconnect_paired_device: no bluetooth handle, skipping\n", core::log_timestamp().c_str());
        return false;
    }
    std::vector<std::string> devices;
    if (!list_paired_devices(h, devices) || devices.empty()) {
        std::printf("%s [BT] auto_reconnect_paired_device: no paired devices to reconnect to\n", core::log_timestamp().c_str());
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
        std::printf("%s [BT] auto_reconnect_paired_device: no valid paired device MAC found, skipping\n", core::log_timestamp().c_str());
        return false;
    }

    std::printf("%s [BT] auto_reconnect_paired_device: reconnecting to '%s'\n", core::log_timestamp().c_str(), connect_id.c_str());
    return connect_device(h, connect_id);
}

std::string get_connected_device_mac() {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection * conn = dbus_connection_open_private(kBluezMonitorBusAddress, &err);
    if (!conn) {
        dbus_error_free(&err);
        return "";
    }
    dbus_error_init(&err);
    if (!dbus_bus_register(conn, &err)) {
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return "";
    }

    std::vector<DeviceSnapshot> devices;
    std::string result;
    if (fetch_managed_devices(conn, devices)) {
        for (const auto & d : devices) {
            if (d.connected) {
                result = d.address;
                break;
            }
        }
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return result;
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

std::string get_bluetooth_hardware_info() {
    std::string mac;
    FILE * f = fopen("/sys/class/bluetooth/hci0/address", "r");
    if (f) {
        char buf[32];
        if (fgets(buf, sizeof(buf), f)) {
            mac = buf;
            while (!mac.empty() && (mac.back() == '\n' || mac.back() == '\r' || mac.back() == ' ')) {
                mac.pop_back();
            }
        }
        fclose(f);
    }
    if (!mac.empty()) {
        return "Realtek RTL8821CS (BlueZ 5.66 / " + mac + ")";
    }
    return "Realtek RTL8821CS (BlueZ 5.66 hci0)";
}

}  // namespace hal
