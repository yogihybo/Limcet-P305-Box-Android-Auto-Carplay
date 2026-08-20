#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dbus/dbus.h>

#define AGENT_PATH "/org/bluez/agent"
#define AGENT_INTERFACE "org.bluez.Agent1"

static DBusHandlerResult agent_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!iface || strcmp(iface, AGENT_INTERFACE) != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    printf("[bt-agent] Received Agent1 method call: %s\n", member);

    DBusMessage *reply = NULL;
    if (strcmp(member, "RequestPinCode") == 0 || strcmp(member, "RequestPasskey") == 0) {
        printf("[bt-agent] Rejecting PIN/Passkey request (NoInputNoOutput mode)\n");
        reply = dbus_message_new_error(msg, "org.bluez.Error.Rejected", "No input capability");
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

static const DBusObjectPathVTable agent_vtable = {
    .message_function = agent_filter,
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

int main(int argc, char *argv[])
{
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

    printf("[bt-agent] Agent active, dispatching pairing events...\n");
    while (dbus_connection_read_write_dispatch(conn, -1)) {
        // Event loop
    }

    return 0;
}
