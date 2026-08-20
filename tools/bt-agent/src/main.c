#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dbus/dbus.h>

#define AGENT_PATH "/org/bluez/agent"
#define AGENT_INTERFACE "org.bluez.Agent1"
#define PROFILE_INTERFACE "org.bluez.Profile1"

// UUID Constants for Automotive Audio and Android Auto
#define UUID_A2DP_SINK    "0000110b-0000-1000-8000-00805f9b34fb"
#define UUID_AVRCP_TARGET "0000110c-0000-1000-8000-00805f9b34fb"
#define UUID_HFP_HF       "0000111e-0000-1000-8000-00805f9b34fb"
#define UUID_ANDROID_AUTO "4de17a00-52cb-11e6-bdf4-0800200c9a66"

static DBusHandlerResult agent_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!iface || strcmp(iface, AGENT_INTERFACE) != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    printf("[bt-agent] Received Agent1 method call: %s\n", member);

    DBusMessage *reply = NULL;
    if (strcmp(member, "RequestPinCode") == 0) {
        printf("[bt-agent] Providing default PIN '0000'\n");
        const char *pin = "0000";
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (strcmp(member, "RequestPasskey") == 0) {
        printf("[bt-agent] Providing default passkey 0\n");
        dbus_uint32_t passkey = 0;
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
    } else {
        // Auto-approve RequestConfirmation, RequestAuthorization, AuthorizeService, DisplayPasskey, etc.
        printf("[bt-agent] Auto-confirming %s\n", member);
        reply = dbus_message_new_method_return(msg);
    }

    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!iface || strcmp(iface, PROFILE_INTERFACE) != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path = dbus_message_get_path(msg);
    printf("[bt-agent] Profile1 event on %s: %s\n", path ? path : "unknown", member);

    if (strcmp(member, "NewConnection") == 0) {
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        const char *dev_path = NULL;
        int fd = -1;
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &dev_path);
        }
        dbus_message_iter_next(&iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
            dbus_message_iter_get_basic(&iter, &fd);
        }
        printf("[bt-agent] *** Profile Connection Established *** on %s from %s (fd=%d)\n",
               path ? path : "", dev_path ? dev_path : "unknown", fd);

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    } else if (strcmp(member, "RequestDisconnection") == 0 || strcmp(member, "Release") == 0) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = {
    .message_function = agent_filter,
};

static const DBusObjectPathVTable profile_vtable = {
    .message_function = profile_filter,
};

static int register_agent(DBusConnection *conn)
{
    DBusError err;
    dbus_error_init(&err);

    if (!dbus_connection_register_object_path(conn, AGENT_PATH, &agent_vtable, NULL)) {
        fprintf(stderr, "[bt-agent] Failed to register object path %s\n", AGENT_PATH);
        return 0;
    }

    // Call RegisterAgent(objpath, capability)
    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                    "org.bluez.AgentManager1", "RegisterAgent");
    if (!msg) return 0;

    const char *path = AGENT_PATH;
    const char *capability = "NoInputNoOutput";
    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[bt-agent] RegisterAgent failed: %s\n", err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);

    // Call RequestDefaultAgent(objpath)
    msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                       "org.bluez.AgentManager1", "RequestDefaultAgent");
    if (!msg) return 0;

    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID);

    reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[bt-agent] RequestDefaultAgent failed: %s\n", err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);

    printf("[bt-agent] Successfully registered as default NoInputNoOutput BlueZ Agent\n");
    return 1;
}

static int register_profile(DBusConnection *conn, const char *path, const char *uuid, const char *name, const char *role, dbus_uint16_t psm, dbus_uint16_t channel)
{
    DBusError err;
    dbus_error_init(&err);

    dbus_connection_register_object_path(conn, path, &profile_vtable, (void *)name);

    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                    "org.bluez.ProfileManager1", "RegisterProfile");
    if (!msg) return 0;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &uuid);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    if (name) {
        DBusMessageIter entry, val;
        const char *key = "Name";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &name);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    if (role) {
        DBusMessageIter entry, val;
        const char *key = "Role";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &role);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    if (psm > 0) {
        DBusMessageIter entry, val;
        const char *key = "PSM";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "q", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_UINT16, &psm);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    if (channel > 0) {
        DBusMessageIter entry, val;
        const char *key = "Channel";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "q", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_UINT16, &channel);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    {
        DBusMessageIter entry, val;
        const char *key = "RequireAuthentication";
        dbus_bool_t req_auth = FALSE;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &req_auth);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    {
        DBusMessageIter entry, val;
        const char *key = "RequireAuthorization";
        dbus_bool_t req_authz = FALSE;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &req_authz);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    {
        DBusMessageIter entry, val;
        const char *key = "AutoConnect";
        dbus_bool_t auto_conn = TRUE;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BOOLEAN, &auto_conn);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&iter, &dict);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[bt-agent] RegisterProfile (%s, UUID: %s) failed: %s\n",
                name ? name : "", uuid, err.message ? err.message : "unknown error");
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);
    printf("[bt-agent] Successfully registered Bluetooth profile '%s' (UUID: %s)\n", name ? name : "", uuid);
    return 1;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (getenv("DBUS_SYSTEM_BUS_ADDRESS") == NULL) {
        if (access("/var/run/run/dbus/system_bus_socket", F_OK) == 0) {
            setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/var/run/run/dbus/system_bus_socket", 1);
        } else if (access("/var/run/dbus/system_bus_socket", F_OK) == 0) {
            setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/var/run/dbus/system_bus_socket", 1);
        }
    }

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        fprintf(stderr, "[bt-agent] Failed to connect to system bus: %s\n", err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return 1;
    }

    // Wait for bluetoothd to be ready on D-Bus
    int registered = 0;
    for (int i = 0; i < 30; ++i) {
        if (register_agent(conn)) {
            registered = 1;
            break;
        }
        sleep(1);
    }

    if (!registered) {
        fprintf(stderr, "[bt-agent] Giving up after 30s\n");
        return 1;
    }

    // Register Audio Profiles so phones discover audio/media services
    register_profile(conn, "/org/bluez/profile/a2dp_sink", UUID_A2DP_SINK, "A2DP Audio Sink", "server", 25, 0);
    register_profile(conn, "/org/bluez/profile/avrcp_target", UUID_AVRCP_TARGET, "A/V Remote Control Target", "server", 23, 0);
    register_profile(conn, "/org/bluez/profile/hfp_hf", UUID_HFP_HF, "Handsfree Audio", "server", 0, 0);

    printf("[bt-agent] Bluetooth stack active with Audio (A2DP/HFP/AVRCP) profiles\n");
    printf("[bt-agent] Dispatching pairing and audio profile events...\n");
    while (dbus_connection_read_write_dispatch(conn, -1)) {
        // Event loop
    }

    return 0;
}
