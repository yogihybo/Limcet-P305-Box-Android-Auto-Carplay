#include "hal/ble_cts.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "core/log_timing.h"

namespace hal {

namespace {

// Matches hal/bluez_aa_profile.cpp's own kBusAddress -- see that
// file's comment. 2026-08-23: fixed to match Buildroot's own real
// dbus-daemon (single /var/run/dbus/), not the old static
// bluetoothd-test daemon's doubled run/run path.
constexpr const char *kBusAddress = "unix:path=/var/run/dbus/system_bus_socket";
constexpr const char *kCtsServiceUuid = "00001805-0000-1000-8000-00805f9b34fb";
constexpr const char *kCtsCurrentTimeCharUuid = "00002a2b-0000-1000-8000-00805f9b34fb";

void log_err(const char *what, const DBusError &err) {
    std::fprintf(stderr, "%s [BT:CTS] %s: %s\n", core::log_timestamp().c_str(), what,
                 err.message ? err.message : "(no message)");
}

std::string mac_to_dbus_path(const std::string &mac) {
    std::string path = "dev_";
    for (char c : mac) {
        if (c == ':') path.push_back('_');
        else path.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return path;
}

// Polls Device1.ServicesResolved -- GATT discovery happens
// asynchronously after connect, so this can't be assumed ready
// immediately. Bounded: a phone with no BLE/GATT support at all (or
// one where this specific connection never brought up an LE link)
// would otherwise hang this forever.
bool wait_for_services_resolved(DBusConnection *conn, const std::string &devicePath, int timeoutSeconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    int attempt = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++attempt;
        DBusMessage *msg = dbus_message_new_method_call(
            "org.bluez", devicePath.c_str(), "org.freedesktop.DBus.Properties", "Get");
        const char *iface = "org.bluez.Device1";
        const char *prop = "ServicesResolved";
        dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                                  DBUS_TYPE_INVALID);
        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 3000, &err);
        dbus_message_unref(msg);
        if (reply) {
            DBusMessageIter iter, variant;
            dbus_message_iter_init(reply, &iter);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&iter, &variant);
                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t resolved = FALSE;
                    dbus_message_iter_get_basic(&variant, &resolved);
                    dbus_message_unref(reply);
                    if (resolved) {
                        std::printf("%s [BT:CTS] ServicesResolved=true after %d check(s)\n",
                                    core::log_timestamp().c_str(), attempt);
                        return true;
                    }
                }
            } else {
                dbus_message_unref(reply);
            }
        } else {
            dbus_error_free(&err);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::printf("%s [BT:CTS] ServicesResolved never became true within %ds (%d checks)\n",
                core::log_timestamp().c_str(), timeoutSeconds, attempt);
    return false;
}

// Walks GetManagedObjects (a{oa{sa{sv}}}) once, looking for a
// GattService1 with UUID 0x1805 nested under devicePath, and a
// GattCharacteristic1 with UUID 0x2A2B nested under THAT service's own
// path (BlueZ nests characteristic object paths under their service's
// path, e.g. .../serviceXXXX/charYYYY -- the prefix check below is
// what actually confirms the characteristic belongs to the right
// service, not just that a same-UUID characteristic exists somewhere).
bool find_cts_characteristic(DBusConnection *conn, const std::string &devicePath,
                              std::string &outCharPath) {
    DBusMessage *msg = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        log_err("GetManagedObjects", err);
        dbus_error_free(&err);
        return false;
    }

    std::string servicePath, charPath;

    DBusMessageIter iter, objects;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&objects, &entry);
        const char *path = nullptr;
        dbus_message_iter_get_basic(&entry, &path);
        dbus_message_iter_next(&entry);

        DBusMessageIter ifaces;
        dbus_message_iter_recurse(&entry, &ifaces);
        while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ifaceEntry;
            dbus_message_iter_recurse(&ifaces, &ifaceEntry);
            const char *ifaceName = nullptr;
            dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);
            dbus_message_iter_next(&ifaceEntry);

            std::string ifaceStr(ifaceName ? ifaceName : "");
            if (ifaceStr == "org.bluez.GattService1" || ifaceStr == "org.bluez.GattCharacteristic1") {
                std::string uuid;
                DBusMessageIter props;
                dbus_message_iter_recurse(&ifaceEntry, &props);
                while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter propEntry;
                    dbus_message_iter_recurse(&props, &propEntry);
                    const char *propName = nullptr;
                    dbus_message_iter_get_basic(&propEntry, &propName);
                    dbus_message_iter_next(&propEntry);
                    if (propName && std::strcmp(propName, "UUID") == 0 &&
                        dbus_message_iter_get_arg_type(&propEntry) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter variant;
                        dbus_message_iter_recurse(&propEntry, &variant);
                        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                            const char *uuidVal = nullptr;
                            dbus_message_iter_get_basic(&variant, &uuidVal);
                            uuid = uuidVal ? uuidVal : "";
                        }
                    }
                    dbus_message_iter_next(&props);
                }

                std::string pathStr(path ? path : "");
                if (ifaceStr == "org.bluez.GattService1" && uuid == kCtsServiceUuid &&
                    pathStr.rfind(devicePath, 0) == 0) {
                    servicePath = pathStr;
                    std::printf("%s [BT:CTS] found Current Time Service at %s\n",
                                core::log_timestamp().c_str(), servicePath.c_str());
                } else if (ifaceStr == "org.bluez.GattCharacteristic1" &&
                           uuid == kCtsCurrentTimeCharUuid) {
                    charPath = pathStr;
                }
            }
            dbus_message_iter_next(&ifaces);
        }
        dbus_message_iter_next(&objects);
    }
    dbus_message_unref(reply);

    if (servicePath.empty()) {
        std::printf("%s [BT:CTS] no Current Time Service (0x1805) found for this device -- "
                    "phone likely doesn't expose GATT CTS\n", core::log_timestamp().c_str());
        return false;
    }
    if (charPath.empty() || charPath.rfind(servicePath, 0) != 0) {
        std::printf("%s [BT:CTS] Current Time Service found but its Current Time "
                    "characteristic (0x2A2B) wasn't -- unusual, treating as unsupported\n",
                    core::log_timestamp().c_str());
        return false;
    }
    std::printf("%s [BT:CTS] found Current Time characteristic at %s\n",
                core::log_timestamp().c_str(), charPath.c_str());
    outCharPath = charPath;
    return true;
}

// Current Time characteristic value (BLE SIG standard layout, 10
// bytes): Year(u16 LE) Month Day Hours Minutes Seconds DayOfWeek
// Fractions256 AdjustReason. Returns empty vector on any D-Bus failure.
std::vector<uint8_t> read_characteristic_value(DBusConnection *conn, const std::string &charPath) {
    DBusMessage *msg = dbus_message_new_method_call(
        "org.bluez", charPath.c_str(), "org.bluez.GattCharacteristic1", "ReadValue");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    // Empty a{sv} options dict -- ReadValue's one required argument,
    // no offset/device options needed for this simple read.
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        log_err("GattCharacteristic1.ReadValue", err);
        dbus_error_free(&err);
        return {};
    }

    std::vector<uint8_t> data;
    DBusMessageIter replyIter;
    dbus_message_iter_init(reply, &replyIter);
    if (dbus_message_iter_get_arg_type(&replyIter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter byteIter;
        dbus_message_iter_recurse(&replyIter, &byteIter);
        while (dbus_message_iter_get_arg_type(&byteIter) == DBUS_TYPE_BYTE) {
            uint8_t b = 0;
            dbus_message_iter_get_basic(&byteIter, &b);
            data.push_back(b);
            dbus_message_iter_next(&byteIter);
        }
    }
    dbus_message_unref(reply);
    return data;
}

}  // namespace

bool sync_clock_via_ble_cts(const std::string &deviceMac) {
    std::printf("%s [BT:CTS] attempting Current Time Service read for %s\n",
                core::log_timestamp().c_str(), deviceMac.c_str());

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_connection_open_private(kBusAddress, &err);
    if (!conn) {
        log_err("dbus_connection_open_private", err);
        dbus_error_free(&err);
        return false;
    }
    dbus_error_init(&err);
    if (!dbus_bus_register(conn, &err)) {
        log_err("dbus_bus_register", err);
        dbus_error_free(&err);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return false;
    }

    std::string devicePath = "/org/bluez/hci0/" + mac_to_dbus_path(deviceMac);
    std::printf("%s [BT:CTS] waiting for GATT services to resolve on %s...\n",
                core::log_timestamp().c_str(), devicePath.c_str());

    bool ok = false;
    if (wait_for_services_resolved(conn, devicePath, 2)) {
        std::string charPath;
        if (find_cts_characteristic(conn, devicePath, charPath)) {
            std::vector<uint8_t> data = read_characteristic_value(conn, charPath);
            if (data.size() >= 7) {
                std::string hex;
                char buf[4];
                for (uint8_t b : data) {
                    std::snprintf(buf, sizeof(buf), "%02x ", b);
                    hex += buf;
                }
                std::printf("%s [BT:CTS] read %zu bytes: %s\n", core::log_timestamp().c_str(),
                            data.size(), hex.c_str());

                uint16_t year = static_cast<uint16_t>(data[0] | (data[1] << 8));
                uint8_t month = data[2], day = data[3], hour = data[4], minute = data[5],
                        second = data[6];

                if (year >= 1970 && year < 2100 && month >= 1 && month <= 12 && day >= 1 &&
                    day <= 31 && hour <= 23 && minute <= 59 && second <= 60) {
                    struct tm tmVal {};
                    tmVal.tm_year = year - 1900;
                    tmVal.tm_mon = month - 1;
                    tmVal.tm_mday = day;
                    tmVal.tm_hour = hour;
                    tmVal.tm_min = minute;
                    tmVal.tm_sec = second;
                    time_t epoch = timegm(&tmVal);
                    std::printf("%s [BT:CTS] parsed date: %04u-%02u-%02u %02u:%02u:%02u UTC "
                                "(epoch=%lld)\n", core::log_timestamp().c_str(), year, month, day,
                                hour, minute, second, static_cast<long long>(epoch));
                    if (epoch > 0) {
                        struct timespec ts {};
                        ts.tv_sec = epoch;
                        if (clock_settime(CLOCK_REALTIME, &ts) == 0) {
                            std::printf("%s [BT:CTS] clock_settime succeeded -- system clock "
                                        "set from BLE CTS\n", core::log_timestamp().c_str());
                            ok = true;
                        } else {
                            std::fprintf(stderr, "%s [BT:CTS] clock_settime failed: %s\n",
                                         core::log_timestamp().c_str(), std::strerror(errno));
                        }
                    }
                } else {
                    std::printf("%s [BT:CTS] parsed date fields look implausible -- not "
                                "setting the clock from this\n", core::log_timestamp().c_str());
                }
            } else {
                std::printf("%s [BT:CTS] ReadValue returned too few bytes (%zu, need >= 7) -- "
                            "not a real Current Time value\n", core::log_timestamp().c_str(),
                            data.size());
            }
        }
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    std::printf("%s [BT:CTS] %s\n", core::log_timestamp().c_str(),
                ok ? "system clock sync via BLE CTS succeeded" : "system clock sync via BLE CTS failed");
    return ok;
}

}  // namespace hal
