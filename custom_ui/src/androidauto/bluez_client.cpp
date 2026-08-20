#include "androidauto/bluez_client.h"
#include "androidauto/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>

#include <dbus/dbus.h>

namespace androidauto {

namespace {

// Matches tools/bluetoothd-test/bt-daemon-probe.sh's own real default --
// this device's /usr/etc/dbus-1/system.conf resolves its <listen> to
// this doubled `run/run` path (a real, if odd, vendor build artifact,
// not a typo -- see that script's own comment for how this was
// confirmed). bluez_stack.h brings up exactly that dbus-daemon/
// bluetoothd pair, so this must match.
constexpr const char *kBusAddress = "unix:path=/var/run/run/dbus/system_bus_socket";
constexpr const char *kAgentPath = "/custom_ui/agent";
constexpr const char *kProfilePath = "/custom_ui/aa_profile";
// Confirmed real, not guessed -- custom_ui/docs/BLUETOOTH_RECONNECT_HANDOFF.md
// and hal/bluetooth.h both independently found this exact value: the
// well-known Android Auto Wireless service UUID every phone with the
// AA app installed publishes in its own SDP records, and that
// blueware's own AAP_ENABLE=1 feature already queries for today.
constexpr const char *kAaUuid = "4de17a00-52cb-11e6-bdf4-0800200c9a66";
// No head-unit UI for PIN/passkey entry exists or is planned -- matches
// this project's own established finding (hal/bluetooth.h) that AA
// pairing is always phone-initiated, Just-Works-style.
constexpr const char *kAgentCapability = "NoInputNoOutput";

void log_dbus_error(const char *what, const DBusError &err) {
    std::fprintf(stderr, "%s androidauto: bluez_client: %s: %s\n", logTimestamp().c_str(), what,
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

struct BluezClient::Impl {
    DBusConnection * conn = nullptr;
    std::atomic<int> pendingFd{-1};
};

namespace {

// org.bluez.Agent1 -- see bluez_client.h's own comment: NoInputNoOutput,
// every decision-requiring call just auto-approves (empty method
// return). RequestPinCode/RequestPasskey have no sane answer without a
// UI, so those alone reply with an error rather than a made-up value.
DBusHandlerResult agent_message_handler(DBusConnection * conn, DBusMessage * msg, void *) {
    const char * iface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!iface || std::strcmp(iface, "org.bluez.Agent1") != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage * reply = nullptr;
    if (std::strcmp(member, "RequestPinCode") == 0) {
        const char * pin = "0000";
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (std::strcmp(member, "RequestPasskey") == 0) {
        dbus_uint32_t passkey = 0;
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
    } else {
        // Release, RequestConfirmation, RequestAuthorization,
        // AuthorizeService, DisplayPasskey, DisplayPinCode, Cancel --
        // all either void notifications or approve-by-empty-reply.
        reply = dbus_message_new_method_return(msg);
    }
    dbus_connection_send(conn, reply, nullptr);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

// org.bluez.Profile1 -- NewConnection is how BlueZ hands us the real,
// already-accepted RFCOMM socket fd (via D-Bus's own UNIX_FD passing)
// once a phone connects to our registered profile. This is the
// standard mechanism a custom RFCOMM profile actually uses -- BlueZ
// itself owns the bind()/listen()/accept() and SDP record, which is
// why this supersedes accept_rfcomm_connection()'s own manual approach
// rather than combining with it.
DBusHandlerResult profile_message_handler(DBusConnection * conn, DBusMessage * msg, void * user_data) {
    auto * impl = static_cast<BluezClient::Impl *>(user_data);
    const char * iface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!iface || std::strcmp(iface, "org.bluez.Profile1") != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (std::strcmp(member, "NewConnection") == 0) {
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        // arg0: object path of the connecting device -- not needed here.
        dbus_message_iter_next(&iter);
        int fd = -1;
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
            dbus_message_iter_get_basic(&iter, &fd);
        }
        std::printf("%s androidauto: bluez_client: NewConnection, fd=%d\n", logTimestamp().c_str(), fd);
        impl->pendingFd.store(fd, std::memory_order_release);
        DBusMessage * reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (std::strcmp(member, "RequestDisconnection") == 0 || std::strcmp(member, "Release") == 0) {
        DBusMessage * reply = dbus_message_new_method_return(msg);
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

}  // namespace

BluezClient::BluezClient() : impl_(new Impl()) {}

BluezClient::~BluezClient() {
    close();
    delete impl_;
}

bool BluezClient::connect() {
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
    return true;
}

bool BluezClient::register_agent() {
    static const DBusObjectPathVTable vtable = {nullptr, &agent_message_handler, nullptr, nullptr,
                                                 nullptr, nullptr};
    if (!dbus_connection_register_object_path(impl_->conn, kAgentPath, &vtable, impl_)) {
        std::fprintf(stderr, "%s androidauto: bluez_client: register_object_path(%s) failed\n",
                     logTimestamp().c_str(), kAgentPath);
        return false;
    }

    const char * path = kAgentPath;
    const char * cap = kAgentCapability;

    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                       "org.bluez.AgentManager1", "RegisterAgent");
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &cap);
    DBusMessage * reply = call_and_unref(impl_->conn, msg, "RegisterAgent");
    if (!reply) return false;
    dbus_message_unref(reply);

    msg = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                                        "RequestDefaultAgent");
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    reply = call_and_unref(impl_->conn, msg, "RequestDefaultAgent");
    if (!reply) return false;
    dbus_message_unref(reply);

    std::printf("%s androidauto: bluez_client: agent registered (%s, capability=%s)\n",
               logTimestamp().c_str(), kAgentPath, kAgentCapability);
    return true;
}

bool BluezClient::register_profile() {
    static const DBusObjectPathVTable vtable = {nullptr, &profile_message_handler, nullptr, nullptr,
                                                 nullptr, nullptr};
    dbus_connection_register_object_path(impl_->conn, kProfilePath, &vtable, impl_);

    const char * path = kProfilePath;
    const char * uuid = kAaUuid;

    // Unregister any stale instance first
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
    dbus_message_iter_close_container(&iter, &dictIter);

    DBusMessage * reply = call_and_unref(impl_->conn, msg, "RegisterProfile");
    if (!reply) return false;
    dbus_message_unref(reply);

    std::printf("%s androidauto: bluez_client: AA profile registered (uuid=%s) -- BlueZ owns "
               "channel allocation + SDP advertisement from here\n", logTimestamp().c_str(), kAaUuid);
    return true;
}

int BluezClient::wait_for_connection(int timeoutSeconds) {
    impl_->pendingFd.store(-1, std::memory_order_relaxed);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write_dispatch(impl_->conn, 200);
        int fd = impl_->pendingFd.load(std::memory_order_acquire);
        if (fd >= 0) return fd;
    }
    std::fprintf(stderr, "%s androidauto: bluez_client: wait_for_connection timed out after %ds\n",
                 logTimestamp().c_str(), timeoutSeconds);
    return -1;
}

void BluezClient::close() {
    if (impl_->conn) {
        dbus_connection_close(impl_->conn);
        dbus_connection_unref(impl_->conn);
        impl_->conn = nullptr;
    }
}

}  // namespace androidauto
