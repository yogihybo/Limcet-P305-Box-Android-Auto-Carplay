#include "hal/bluez_aa_profile.h"
#include "core/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>

#include <dbus/dbus.h>

namespace hal {

namespace {

// 2026-08-23: real hw bug -- this doubled `run/run` path matched the
// OLD static bluetoothd-test dbus-daemon's own compiled-in default
// (tools/bluetoothd-test/bt-daemon-probe.sh). rcS now starts
// Buildroot's own dbus-daemon package instead, whose real config
// (/usr/share/dbus-1/system.conf's <listen>, confirmed by direct
// read) uses the standard single `/var/run/dbus/` path. Must match
// whatever daemon rcS actually starts.
constexpr const char *kBusAddress = "unix:path=/var/run/dbus/system_bus_socket";
constexpr const char *kProfilePath = "/custom_ui/aa_profile";
// Confirmed real, not guessed -- custom_ui/docs/BLUETOOTH_RECONNECT_HANDOFF.md
// and hal/bluetooth.h both independently found this exact value: the
// well-known Android Auto Wireless service UUID every phone with the
// AA app installed publishes in its own SDP records.
constexpr const char *kAaUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

void log_dbus_error(const char *what, const DBusError &err) {
    if (err.name && (std::strcmp(err.name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0 ||
                     std::strcmp(err.name, "org.bluez.Error.DoesNotExist") == 0)) {
        return; // Expected while BlueZ daemon is still initializing on boot
    }
    std::fprintf(stderr, "%s [BT] %s: %s\n", core::log_timestamp().c_str(), what,
                 err.message ? err.message : "(no message)");
}

// Sends a method call with no arguments (or args already appended by
// the caller before this runs) and blocks for the reply. Returns
// nullptr on failure (frees msg either way).
DBusMessage * call_and_unref(DBusConnection * conn, DBusMessage * msg, const char * what) {
    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        log_dbus_error(what, err);
        dbus_error_free(&err);
        return nullptr;
    }
    return reply;
}

}  // namespace

struct BluezAaProfile::Impl {
    DBusConnection * conn = nullptr;
    std::atomic<int> pendingFd{-1};
    bool pathRegistered = false;
};

namespace {

// org.bluez.Profile1 -- NewConnection is how BlueZ hands us the real,
// already-accepted RFCOMM socket fd (via D-Bus's own UNIX_FD passing)
// once a phone connects to our registered profile. This is the
// standard mechanism a custom RFCOMM profile actually uses -- BlueZ
// itself owns the bind()/listen()/accept() and SDP record, which is
// why this supersedes accept_rfcomm_connection()'s own manual approach
// rather than combining with it.
DBusHandlerResult profile_message_handler(DBusConnection * conn, DBusMessage * msg, void * user_data) {
    auto * impl = static_cast<BluezAaProfile::Impl *>(user_data);
    const char * iface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!iface || std::strcmp(iface, "org.bluez.Profile1") != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    std::printf("%s [BT] Profile1 method call received: %s\n",
                core::log_timestamp().c_str(), member);

    if (std::strcmp(member, "NewConnection") == 0) {
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        const char * devicePath = nullptr;
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &devicePath);
        }
        dbus_message_iter_next(&iter);
        int fd = -1;
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
            dbus_message_iter_get_basic(&iter, &fd);
        }
        std::printf("%s [BT] *** NewConnection *** device=%s fd=%d\n",
                    core::log_timestamp().c_str(), devicePath ? devicePath : "(unknown)", fd);
        impl->pendingFd.store(fd, std::memory_order_release);
        DBusMessage * reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (std::strcmp(member, "RequestDisconnection") == 0 || std::strcmp(member, "Release") == 0) {
        std::printf("%s [BT] %s\n", core::log_timestamp().c_str(), member);
        DBusMessage * reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

}  // namespace

BluezAaProfile::BluezAaProfile() : impl_(new Impl()) {}

BluezAaProfile::~BluezAaProfile() {
    close();
    delete impl_;
}

bool BluezAaProfile::connect() {
    DBusError err;
    dbus_error_init(&err);
    impl_->conn = dbus_connection_open_private(kBusAddress, &err);
    if (!impl_->conn) {
        log_dbus_error("dbus_connection_open_private", err);
        dbus_error_free(&err);
        return false;
    }
    dbus_error_init(&err);
    if (!dbus_bus_register(impl_->conn, &err)) {
        log_dbus_error("dbus_bus_register", err);
        dbus_error_free(&err);
        dbus_connection_close(impl_->conn);
        dbus_connection_unref(impl_->conn);
        impl_->conn = nullptr;
        return false;
    }
    std::printf("%s [BT] connected to system bus (%s)\n",
                core::log_timestamp().c_str(), kBusAddress);
    return true;
}

bool BluezAaProfile::register_profile() {
    if (!impl_->pathRegistered) {
        static const DBusObjectPathVTable vtable = {nullptr, &profile_message_handler, nullptr, nullptr,
                                                     nullptr, nullptr};
        dbus_connection_register_object_path(impl_->conn, kProfilePath, &vtable, impl_);
        impl_->pathRegistered = true;
    }

    const char * path = kProfilePath;
    const char * uuid = kAaUuid;

    // Unregister any stale instance first (e.g. left over from a
    // previous custom_ui run that didn't exit cleanly).
    {
        DBusMessage * unreg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                           "org.bluez.ProfileManager1", "UnregisterProfile");
        if (unreg) {
            dbus_message_append_args(unreg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
            DBusMessage * reply = call_and_unref(impl_->conn, unreg, "UnregisterProfile");
            if (reply) dbus_message_unref(reply);
        }
    }

    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                       "org.bluez.ProfileManager1", "RegisterProfile");
    DBusMessageIter iter, dictIter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dictIter);
    {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "Name";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char * val = "Android Auto";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "Role";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        const char * val = "server";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "Channel";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "q", &variant);
        dbus_uint16_t val = 1;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT16, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "AutoConnect";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = TRUE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    {
        // Matches how AA pairing already works on this project (phone-
        // initiated, no head-unit-driven auth step) -- doesn't gate the
        // RFCOMM connection itself behind a second authentication round.
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "RequireAuthentication";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    {
        DBusMessageIter entry, variant;
        dbus_message_iter_open_container(&dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char * key = "RequireAuthorization";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t val = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dictIter, &entry);
    }
    dbus_message_iter_close_container(&iter, &dictIter);

    DBusMessage * reply = call_and_unref(impl_->conn, msg, "RegisterProfile");
    if (!reply) return false;
    dbus_message_unref(reply);

    std::printf("%s [BT] AA profile registered (uuid=%s, path=%s) -- BlueZ owns "
               "channel allocation + SDP advertisement from here\n", core::log_timestamp().c_str(),
               kAaUuid, kProfilePath);
    return true;
}

int BluezAaProfile::wait_for_connection(int timeoutSeconds) {
    impl_->pendingFd.store(-1, std::memory_order_relaxed);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write_dispatch(impl_->conn, 200);
        int fd = impl_->pendingFd.load(std::memory_order_acquire);
        if (fd >= 0) return fd;
    }
    return -1;
}

void BluezAaProfile::close() {
    if (impl_->conn) {
        dbus_connection_close(impl_->conn);
        dbus_connection_unref(impl_->conn);
        impl_->conn = nullptr;
    }
}

}  // namespace hal
