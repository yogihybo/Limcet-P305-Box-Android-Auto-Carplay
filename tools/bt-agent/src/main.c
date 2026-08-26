/*
 * bt-agent: Dedicated auto-pairing agent and A2DP SBC Sink Audio Player
 *           for BlueZ 5 on embedded ARK1680.
 *
 * Exposes:
 *  1. org.bluez.Agent1 on /org/bluez/agent (NoInputNoOutput auto-pairing)
 *  2. org.bluez.MediaEndpoint1 on /org/bluez/endpoint/a2dp_sink (A2DP SBC Sink)
 *  3. Acquires MediaTransport1 and streams decoded SBC audio directly to ALSA.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <dbus/dbus.h>
#include <alsa/asoundlib.h>
#include "sbc.h"

#define AGENT_PATH "/org/bluez/agent"
#define AGENT_INTERFACE "org.bluez.Agent1"
#define ENDPOINT_PATH "/org/bluez/endpoint/a2dp_sink"
#define ENDPOINT_INTERFACE "org.bluez.MediaEndpoint1"
#define UUID_A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"

// SBC Capabilities: 44.1/48kHz, Mono/Dual/Stereo/JointStereo, 4-16 blocks, 8 subbands, SNR/Loudness, bitpool 2-53
static const uint8_t sbc_capabilities[] = {
    0x3f,       // 16kHz, 32kHz, 44.1kHz, 48kHz | Mono, Dual Channel, Stereo, Joint Stereo
    0xff,       // 4, 8, 12, 16 blocks | 4, 8 subbands | SNR, Loudness
    0x02,       // Min bitpool: 2
    0x35        // Max bitpool: 53
};

static pthread_t g_audio_thread;
static volatile int g_audio_running = 0;
static char g_current_transport[256] = {0};

static void *audio_playback_thread(void *arg)
{
    char *transport_path = (char *)arg;
    printf("[BT:AGENT] Starting A2DP Audio Playback Worker for %s\n", transport_path);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        fprintf(stderr, "[BT:AGENT] audio worker: failed to get system bus: %s\n",
                err.message ? err.message : "unknown");
        free(transport_path);
        return NULL;
    }

    DBusMessage *msg = dbus_message_new_method_call("org.bluez", transport_path,
                                                    "org.bluez.MediaTransport1", "Acquire");
    if (!msg) {
        dbus_connection_unref(conn);
        free(transport_path);
        return NULL;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        fprintf(stderr, "[BT:AGENT] MediaTransport1.Acquire failed: %s\n",
                err.message ? err.message : "unknown");
        dbus_error_free(&err);
        dbus_connection_unref(conn);
        free(transport_path);
        return NULL;
    }

    int transport_fd = -1;
    uint16_t read_mtu = 0, write_mtu = 0;
    DBusMessageIter iter;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UNIX_FD) {
        dbus_message_iter_get_basic(&iter, &transport_fd);
    }
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT16) {
        dbus_message_iter_get_basic(&iter, &read_mtu);
    }
    dbus_message_iter_next(&iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT16) {
        dbus_message_iter_get_basic(&iter, &write_mtu);
    }
    dbus_message_unref(reply);

    printf("[BT:AGENT] MediaTransport1 acquired: fd=%d, read_mtu=%u, write_mtu=%u\n",
           transport_fd, (unsigned)read_mtu, (unsigned)write_mtu);

    if (transport_fd < 0) {
        dbus_connection_unref(conn);
        free(transport_path);
        return NULL;
    }

    // Open ALSA PCM device (try plug:softvol2 then default)
    snd_pcm_t *pcm = NULL;
    int alsa_err = snd_pcm_open(&pcm, "plug:softvol2", SND_PCM_STREAM_PLAYBACK, 0);
    if (alsa_err < 0) {
        alsa_err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    }
    if (alsa_err < 0) {
        fprintf(stderr, "[BT:AGENT] ALSA snd_pcm_open failed: %s\n", snd_strerror(alsa_err));
        close(transport_fd);
        dbus_connection_unref(conn);
        free(transport_path);
        return NULL;
    }

    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       2, 44100, 1, 200000);

    sbc_t sbc;
    sbc_init(&sbc, 0);

    g_audio_running = 1;
    uint8_t packet[2048];
    uint8_t pcm_buf[4096];

    while (g_audio_running) {
        ssize_t n = read(transport_fd, packet, sizeof(packet));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
        if (n < 13) continue; // 12 bytes RTP header + 1 byte SBC payload header

        uint8_t *sbc_data = packet + 13;
        size_t sbc_len = n - 13;

        while (sbc_len > 0 && g_audio_running) {
            size_t pcm_written = 0;
            ssize_t decoded = sbc_decode(&sbc, sbc_data, sbc_len, pcm_buf, sizeof(pcm_buf), &pcm_written);
            if (decoded <= 0) break;
            sbc_data += decoded;
            sbc_len -= decoded;

            if (pcm_written > 0) {
                snd_pcm_sframes_t frames = snd_pcm_writei(pcm, pcm_buf, pcm_written / 4);
                if (frames < 0) {
                    snd_pcm_recover(pcm, frames, 1);
                }
            }
        }
    }

    printf("[BT:AGENT] Audio playback worker exiting for %s\n", transport_path);
    if (pcm) {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
    }
    sbc_finish(&sbc);
    close(transport_fd);

    // Call Release on transport
    msg = dbus_message_new_method_call("org.bluez", transport_path,
                                       "org.bluez.MediaTransport1", "Release");
    if (msg) {
        reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
        if (reply) dbus_message_unref(reply);
        dbus_message_unref(msg);
        dbus_error_free(&err);
    }
    dbus_connection_unref(conn);
    free(transport_path);
    return NULL;
}

static DBusHandlerResult agent_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);

    if (!iface || strcmp(iface, AGENT_INTERFACE) != 0 || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = NULL;

    if (strcmp(member, "RequestPinCode") == 0) {
        const char *pin = "0000";
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (strcmp(member, "RequestPasskey") == 0) {
        uint32_t passkey = 0;
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
    } else if (strcmp(member, "DisplayPinCode") == 0 ||
               strcmp(member, "DisplayPasskey") == 0 ||
               strcmp(member, "RequestConfirmation") == 0 ||
               strcmp(member, "RequestAuthorization") == 0 ||
               strcmp(member, "AuthorizeService") == 0) {
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(member, "Cancel") == 0 || strcmp(member, "Release") == 0) {
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

    printf("[BT:AGENT] MediaEndpoint1 method call: %s\n", member);

    DBusMessage *reply = NULL;
    if (strcmp(member, "SetConfiguration") == 0) {
        const char *transport_path = NULL;
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&iter, &transport_path);
        }
        printf("[BT:AGENT] *** A2DP Audio Stream Configured ***: transport=%s\n",
               transport_path ? transport_path : "unknown");
        reply = dbus_message_new_method_return(msg);

        if (transport_path) {
            strncpy(g_current_transport, transport_path, sizeof(g_current_transport) - 1);
            if (g_audio_running) {
                g_audio_running = 0;
                pthread_join(g_audio_thread, NULL);
            }
            char *path_copy = strdup(transport_path);
            pthread_create(&g_audio_thread, NULL, audio_playback_thread, path_copy);
        }
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
        printf("[BT:AGENT] A2DP Codec Configuration Selected (44.1kHz Stereo)\n");
    } else if (strcmp(member, "ClearConfiguration") == 0) {
        printf("[BT:AGENT] A2DP Audio Stream Cleared\n");
        if (g_audio_running) {
            g_audio_running = 0;
            pthread_join(g_audio_thread, NULL);
        }
        reply = dbus_message_new_method_return(msg);
    } else if (strcmp(member, "Release") == 0) {
        printf("[BT:AGENT] MediaEndpoint Released\n");
        if (g_audio_running) {
            g_audio_running = 0;
            pthread_join(g_audio_thread, NULL);
        }
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
    static int agent_path_registered = 0;
    if (!agent_path_registered) {
        if (!dbus_connection_register_object_path(conn, AGENT_PATH, &agent_vtable, NULL)) {
            fprintf(stderr, "[BT:AGENT] Failed to register object path %s\n", AGENT_PATH);
            return 0;
        }
        agent_path_registered = 1;
    }

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
                                                    "org.bluez.AgentManager1", "RegisterAgent");
    if (!msg) return 0;

    const char *path = AGENT_PATH;
    const char *capability = "NoInputNoOutput";
    dbus_message_append_args(msg,
                             DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_STRING, &capability,
                             DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 3000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (!dbus_error_has_name(&err, "org.freedesktop.DBus.Error.ServiceUnknown") &&
            !dbus_error_has_name(&err, "org.bluez.Error.AlreadyExists")) {
            fprintf(stderr, "[BT:AGENT] RegisterAgent failed: %s\n", err.message ? err.message : "unknown");
        }
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

    reply = dbus_connection_send_with_reply_and_block(conn, msg, 3000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (!dbus_error_has_name(&err, "org.freedesktop.DBus.Error.ServiceUnknown")) {
            fprintf(stderr, "[BT:AGENT] RequestDefaultAgent failed: %s\n", err.message ? err.message : "unknown");
        }
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);

    printf("[BT:AGENT] Successfully registered as default NoInputNoOutput BlueZ Agent\n");
    return 1;
}

static int register_media_endpoint(DBusConnection *conn)
{
    static int endpoint_path_registered = 0;
    if (!endpoint_path_registered) {
        if (!dbus_connection_register_object_path(conn, ENDPOINT_PATH, &endpoint_vtable, NULL)) {
            fprintf(stderr, "[BT:AGENT] Failed to register endpoint object path %s\n", ENDPOINT_PATH);
            return 0;
        }
        endpoint_path_registered = 1;
    }

    DBusError err;
    dbus_error_init(&err);

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

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 3000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (!dbus_error_has_name(&err, "org.freedesktop.DBus.Error.ServiceUnknown") &&
            !dbus_error_has_name(&err, "org.bluez.Error.AlreadyExists")) {
            fprintf(stderr, "[BT:AGENT] RegisterEndpoint (A2DP SBC Sink) failed: %s\n",
                    err.message ? err.message : "unknown error");
        }
        dbus_error_free(&err);
        return 0;
    }
    dbus_message_unref(reply);
    printf("[BT:AGENT] Successfully registered Media Endpoint: A2DP SBC Sink on /org/bluez/hci0\n");
    return 1;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
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
    DBusConnection *conn = NULL;

    // Retry connecting to D-Bus system bus
    for (int retry = 0; retry < 30; ++retry) {
        conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
        if (conn) break;
        dbus_error_free(&err);
        dbus_error_init(&err);
        usleep(500000); // 500ms
    }

    if (!conn) {
        fprintf(stderr, "[BT:AGENT] Failed to connect to system bus: %s\n", err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return 1;
    }

    // Retry registering agent until bluetoothd is active on D-Bus
    int agent_registered = 0;
    for (int retry = 0; retry < 60; ++retry) {
        if (register_agent(conn)) {
            agent_registered = 1;
            break;
        }
        usleep(500000); // 500ms
    }

    if (!agent_registered) {
        fprintf(stderr, "[BT:AGENT] Failed to register agent after retries\n");
        return 1;
    }

    // Retry registering media endpoint until BlueZ adapter is ready
    int endpoint_registered = 0;
    for (int retry = 0; retry < 60; ++retry) {
        if (register_media_endpoint(conn)) {
            endpoint_registered = 1;
            break;
        }
        usleep(500000); // 500ms
    }

    if (!endpoint_registered) {
        fprintf(stderr, "[BT:AGENT] Failed to register A2DP media endpoint after retries\n");
        return 1;
    }

    printf("[BT:AGENT] Bluetooth stack active with Auto-Pairing Agent + A2DP Media Endpoint (Audio streaming to ALSA ready)\n");
    printf("[BT:AGENT] Dispatching pairing and media events...\n");

    while (dbus_connection_read_write_dispatch(conn, -1)) {
        // Event loop running
    }

    return 0;
}
