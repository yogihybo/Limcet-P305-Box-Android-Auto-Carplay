#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dbus/dbus.h>

#define AGENT_PATH "/org/bluez/agent"
#define AGENT_INTERFACE "org.bluez.Agent1"

#define ENDPOINT_PATH "/org/bluez/endpoint/sbc_sink"
#define ENDPOINT_INTERFACE "org.bluez.MediaEndpoint1"

#define UUID_A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"

// SBC Capabilities: 16k-48k, Mono/Dual/Stereo/JointStereo, 4-16 blocks, 4-8 subbands, Loudness/SNR, bitpool 2-53
static const uint8_t sbc_capabilities[] = { 0x3f, 0xff, 0x02, 0x35 };

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

static DBusHandlerResult endpoint_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!iface || strcmp(iface, ENDPOINT_INTERFACE) != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    printf("[bt-agent] MediaEndpoint1 method call: %s\n", member);

    DBusMessage *reply = NULL;
    if (strcmp(member, "SetConfiguration") == 0) {
        const char *transport_path = NULL;
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &transport_path);
        }
        printf("[bt-agent] *** A2DP Audio Stream Configured ***: transport=%s\n",
               transport_path ? transport_path : "unknown");
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(member, "SelectConfiguration") == 0) {
        // Return 44.1kHz, Joint Stereo, 16 blocks, 8 subbands, Loudness, 2-53 bitpool
        const uint8_t selected_sbc[] = { 0x21, 0x15, 0x02, 0x35 };
        reply = dbus_message_new_method_return(msg);
        DBusMessageIter iter, array_iter;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "y", &array_iter);
        for (size_t i = 0; i < sizeof(selected_sbc); ++i) {
            dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &selected_sbc[i]);
        }
        dbus_message_iter_close_container(&iter, &array_iter);
        printf("[bt-agent] A2DP Codec Configuration Selected (44.1kHz Stereo)\n");
    } else if (strcmp(member, "ClearConfiguration") == 0) {
        printf("[bt-agent] A2DP Audio Stream Cleared\n");
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(member, "Release") == 0) {
        printf("[bt-agent] MediaEndpoint Released\n");
        reply = dbus_message_new_method_return(msg);
    }

    if (reply) {
        dbus_connection_send(conn, reply, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = {
    .message_function = agent_filter,
};

static const DBusObjectPathVTable endpoint_vtable = {
    .message_function = endpoint_filter,
};

static int register_agent(DBusConnection *conn)
{
    DBusError err;
    dbus_error_init(&err);

    if (!dbus_connection_register_object_path(conn, AGENT_PATH, &agent_vtable, NULL)) {
        fprintf(stderr, "[bt-agent] Failed to register object path %s\n", AGENT_PATH);
        return 0;
    }

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

static int register_media_endpoint(DBusConnection *conn)
{
    DBusError err;
    dbus_error_init(&err);

    dbus_connection_register_object_path(conn, ENDPOINT_PATH, &endpoint_vtable, NULL);

    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0",
                                                    "org.bluez.Media1", "RegisterEndpoint");
    if (!msg) return 0;

    const char *path = ENDPOINT_PATH;
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);

    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    // UUID
    {
        DBusMessageIter entry, val;
        const char *key = "UUID";
        const char *uuid = UUID_A2DP_SINK;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &uuid);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    // Codec
    {
        DBusMessageIter entry, val;
        const char *key = "Codec";
        uint8_t codec = 0x00; // SBC
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "y", &val);
        dbus_message_iter_append_basic(&val, DBUS_TYPE_BYTE, &codec);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }
    // Capabilities
    {
        DBusMessageIter entry, val, array_iter;
        const char *key = "Capabilities";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &val);
        dbus_message_iter_open_container(&val, DBUS_TYPE_ARRAY, "y", &array_iter);
        for (size_t i = 0; i < sizeof(sbc_capabilities); ++i) {
            dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_BYTE, &sbc_capabilities[i]);
        }
        dbus_message_iter_close_container(&val, &array_iter);
        dbus_message_iter_close_container(&entry, &val);
        dbus_message_iter_close_container(&dict, &entry);
    }

    dbus_message_iter_close_container(&iter, &dict);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[bt-agent] RegisterEndpoint (A2DP SBC Sink) failed: %s\n",
                err.message ? err.message : "unknown error");
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);
    printf("[bt-agent] Successfully registered Media Endpoint: A2DP SBC Sink on /org/bluez/hci0\n");
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

    // Register A2DP SBC Media Endpoint
    register_media_endpoint(conn);

    printf("[bt-agent] Bluetooth stack active with Auto-Pairing Agent + A2DP Media Endpoint\n");
    printf("[bt-agent] Dispatching pairing and media events...\n");
    while (dbus_connection_read_write_dispatch(conn, -1)) {
        // Event loop
    }

    return 0;
}
