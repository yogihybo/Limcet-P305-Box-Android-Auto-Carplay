#include "aap_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <pb_encode.h>
#include <pb_decode.h>

#include "aap_protobuf/service/control/ControlMessageType.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/control/message/ByeByeRequest.pb.h"
#include "aap_protobuf/service/control/message/ByeByeResponse.pb.h"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequest.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/VoiceSessionNotification.pb.h"

#include "aap_protobuf/service/media/sink/MediaMessageId.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/shared/message/Stop.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusMode.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusReason.pb.h"

#include "aap_protobuf/service/inputsource/InputMessageId.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h"

#include "aap_protobuf/service/sensorsource/SensorMessageId.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"
#include "aap_protobuf/service/bluetooth/BluetoothMessageId.pb.h"
#include "aap_protobuf/service/bluetooth/message/BluetoothPairingRequest.pb.h"
#include "aap_protobuf/service/bluetooth/message/BluetoothPairingResponse.pb.h"
#include "aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h"
#include "aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h"
#include "aap_microphone.h"

#define RX_BUFFER_SIZE (64 * 1024)
#define TX_BUFFER_SIZE (64 * 1024)

#define FALLBACK_BASELINE_EPOCH_MS 1755043200000LL // ~2026-08-13 UTC

static const char *channel_name(uint8_t ch) {
    switch (ch) {
        case AAP_CHANNEL_CONTROL: return "control";
        case AAP_CHANNEL_SENSOR: return "sensor";
        case AAP_CHANNEL_MEDIA_SINK_VIDEO: return "video";
        case AAP_CHANNEL_MEDIA_SINK_MEDIA_AUDIO: return "audio (plug:softvol2)";
        case AAP_CHANNEL_MEDIA_SINK_GUIDANCE_AUDIO: return "audio (plug:softvol1)";
        case AAP_CHANNEL_MEDIA_SINK_SYSTEM_AUDIO: return "audio (plug:softvol4)";
        case AAP_CHANNEL_INPUT: return "input";
        case AAP_CHANNEL_MICROPHONE: return "microphone";
        case AAP_CHANNEL_BLUETOOTH: return "bluetooth";
        default: return "unknown";
    }
}

static int64_t plausible_epoch_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t elapsed = (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
    return FALLBACK_BASELINE_EPOCH_MS + elapsed;
}

static uint64_t now_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#define AAP_MAX_CHANNELS 16

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t capacity;
    uint8_t flags;
} aap_channel_assembler_t;

struct aap_session {
    int socket_fd;
    aap_session_state_t state;
    char status_message[128];

    aap_cryptor_t *cryptor;
    aap_audio_sink_t *audio_media;
    aap_audio_sink_t *audio_guidance;
    aap_audio_sink_t *audio_system;
    aap_video_sink_t *video_sink;
    aap_microphone_t *mic;

    aap_channel_assembler_t assemblers[AAP_MAX_CHANNELS];

    uint8_t rx_buf[RX_BUFFER_SIZE];
    size_t rx_len;

    int32_t media_session_id;
    int32_t video_session_id;

    time_t last_ping_time;
    time_t last_rx_time;

    bool is_video_focus_native;
};

static void set_state(aap_session_t *s, aap_session_state_t state, const char *msg) {
    s->state = state;
    strncpy(s->status_message, msg ? msg : "", sizeof(s->status_message) - 1);
    printf("[AA] [%d] %s\n", state, s->status_message);
}

static bool send_raw_frame(aap_session_t *s, uint8_t channel_id, aap_frame_type_t frame_type,
                           bool is_control, bool is_encrypted,
                           const uint8_t *payload, size_t payload_len) {
    if (s->socket_fd < 0) return false;

    uint8_t header[AAP_HEADER_EXTENDED_SIZE];
    size_t hdr_len = aap_pack_frame_header(channel_id, frame_type, is_control, is_encrypted,
                                           (uint16_t)payload_len, (uint32_t)payload_len,
                                           header, sizeof(header));
    if (hdr_len == 0) return false;

    ssize_t n1 = write(s->socket_fd, header, hdr_len);
    if (n1 != (ssize_t)hdr_len) return false;

    if (payload && payload_len > 0) {
        ssize_t n2 = write(s->socket_fd, payload, payload_len);
        if (n2 != (ssize_t)payload_len) return false;
    }
    return true;
}

static bool send_channel_msg(aap_session_t *s, uint8_t channel_id, uint16_t msg_id,
                             const uint8_t *proto_data, size_t proto_len, bool encrypt) {
    uint8_t raw_msg[4096];
    raw_msg[0] = (uint8_t)(msg_id >> 8);
    raw_msg[1] = (uint8_t)(msg_id & 0xFF);
    if (proto_data && proto_len > 0) {
        if (proto_len + 2 > sizeof(raw_msg)) return false;
        memcpy(raw_msg + 2, proto_data, proto_len);
    }
    size_t total_len = proto_len + 2;

    if (encrypt) {
        uint8_t enc_buf[4096 + 64];
        size_t enc_len = aap_cryptor_encrypt(s->cryptor, raw_msg, total_len, enc_buf, sizeof(enc_buf));
        if (enc_len == 0) return false;
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, false, true, enc_buf, enc_len);
    } else {
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, false, false, raw_msg, total_len);
    }
}

static bool send_channel_control_msg(aap_session_t *s, uint8_t channel_id, uint16_t msg_id,
                                     const uint8_t *proto_data, size_t proto_len, bool encrypt) {
    uint8_t raw_msg[4096];
    raw_msg[0] = (uint8_t)(msg_id >> 8);
    raw_msg[1] = (uint8_t)(msg_id & 0xFF);
    if (proto_data && proto_len > 0) {
        if (proto_len + 2 > sizeof(raw_msg)) return false;
        memcpy(raw_msg + 2, proto_data, proto_len);
    }
    size_t total_len = proto_len + 2;

    if (encrypt && aap_cryptor_is_active(s->cryptor)) {
        uint8_t cipher[4096];
        size_t cipher_len = aap_cryptor_encrypt(s->cryptor, raw_msg, total_len, cipher, sizeof(cipher));
        if (cipher_len == 0) return false;
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, true, true, cipher, cipher_len);
    } else {
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, true, false, raw_msg, total_len);
    }
}

static bool send_media_ack(aap_session_t *s, uint8_t channel_id, int32_t session_id, uint32_t ack_tokens) {
    aap_protobuf_service_media_source_message_Ack ack = aap_protobuf_service_media_source_message_Ack_init_default;
    ack.session_id = session_id;
    ack.has_ack = true;
    ack.ack = ack_tokens;

    uint8_t pb_buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&stream, aap_protobuf_service_media_source_message_Ack_fields, &ack)) {
        return false;
    }

    return send_channel_msg(s, channel_id, aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_ACK,
                            pb_buf, stream.bytes_written, true);
}

static void send_version_request(aap_session_t *s) {
    uint8_t ver_req[4];
    uint16_t major = htons(1);
    uint16_t minor = htons(6);
    memcpy(&ver_req[0], &major, 2);
    memcpy(&ver_req[2], &minor, 2);

    printf("[AA] sending VersionRequest (1.6)\n");
    send_channel_msg(s, AAP_CHANNEL_CONTROL, aap_protobuf_service_control_message_ControlMessageType_MESSAGE_VERSION_REQUEST,
                     ver_req, sizeof(ver_req), false);
    set_state(s, AAP_SESSION_STATE_VERSION_HANDSHAKE, "VersionRequest sent, waiting for VersionResponse");
}

static void handle_channel_open_request(aap_session_t *s, uint8_t channel_id, const uint8_t *payload, size_t payload_len) {
    aap_protobuf_service_control_message_ChannelOpenRequest req =
        aap_protobuf_service_control_message_ChannelOpenRequest_init_default;
    if (payload_len > 0) {
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
        pb_decode(&stream, aap_protobuf_service_control_message_ChannelOpenRequest_fields, &req);
    }

    printf("[AA] channel %u (%s) open request (priority=%d)\n", channel_id, channel_name(channel_id), req.priority);

    aap_protobuf_service_control_message_ChannelOpenResponse resp =
        aap_protobuf_service_control_message_ChannelOpenResponse_init_default;
    resp.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;

    uint8_t pb_buf[128];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_control_message_ChannelOpenResponse_fields, &resp);

    send_channel_control_msg(s, channel_id,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_RESPONSE,
                             pb_buf, ostream.bytes_written, true);
    printf("[AA] channel %u (%s) open response sent (STATUS_SUCCESS)\n", channel_id, channel_name(channel_id));
}

static void handle_control_message(aap_session_t *s, uint16_t msg_id, const uint8_t *payload, size_t payload_len) {
    switch (msg_id) {
        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_VERSION_RESPONSE: {
            printf("[AA] received VersionResponse (%zu bytes)\n", payload_len);
            set_state(s, AAP_SESSION_STATE_TLS_HANDSHAKE, "Version handshake complete, starting TLS");

            /* Kick off TLS Client Hello */
            aap_cryptor_do_handshake(s->cryptor);
            uint8_t hs_buf[2048];
            size_t hs_len = aap_cryptor_read_handshake(s->cryptor, hs_buf, sizeof(hs_buf));
            if (hs_len > 0) {
                printf("[AA] sending ClientHello (%zu bytes)\n", hs_len);
                send_channel_msg(s, AAP_CHANNEL_CONTROL,
                                 aap_protobuf_service_control_message_ControlMessageType_MESSAGE_ENCAPSULATED_SSL,
                                 hs_buf, hs_len, false);
            }
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_ENCAPSULATED_SSL: {
            printf("[AA] received SSLHandshake (%zu bytes)\n", payload_len);
            aap_cryptor_write_handshake(s->cryptor, payload, payload_len);
            bool done = aap_cryptor_do_handshake(s->cryptor);

            uint8_t hs_buf[2048];
            size_t hs_len = aap_cryptor_read_handshake(s->cryptor, hs_buf, sizeof(hs_buf));
            if (hs_len > 0) {
                send_channel_msg(s, AAP_CHANNEL_CONTROL,
                                 aap_protobuf_service_control_message_ControlMessageType_MESSAGE_ENCAPSULATED_SSL,
                                 hs_buf, hs_len, false);
            }

            if (done && aap_cryptor_is_active(s->cryptor)) {
                printf("[AA] TLS handshake completed successfully! Sending AuthComplete\n");
                set_state(s, AAP_SESSION_STATE_AUTH, "TLS complete, authenticating");

                aap_protobuf_service_control_message_AuthResponse auth =
                    aap_protobuf_service_control_message_AuthResponse_init_default;
                auth.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;

                uint8_t pb_buf[128];
                pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
                pb_encode(&stream, aap_protobuf_service_control_message_AuthResponse_fields, &auth);

                send_channel_msg(s, AAP_CHANNEL_CONTROL,
                                 aap_protobuf_service_control_message_ControlMessageType_MESSAGE_AUTH_COMPLETE,
                                 pb_buf, stream.bytes_written, false);
            }
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_SERVICE_DISCOVERY_REQUEST: {
            printf("[AA] received ServiceDiscoveryRequest\n");

            aap_protobuf_service_control_message_ServiceDiscoveryResponse resp =
                aap_protobuf_service_control_message_ServiceDiscoveryResponse_init_default;

            resp.has_make = true;
            strncpy(resp.make, "custom_ui", sizeof(resp.make) - 1);
            resp.has_model = true;
            strncpy(resp.model, "prado-firmware-reconstruction", sizeof(resp.model) - 1);
            resp.has_year = true;
            strncpy(resp.year, "2026", sizeof(resp.year) - 1);
            resp.has_vehicle_id = true;
            strncpy(resp.vehicle_id, "prado-custom-ui-001", sizeof(resp.vehicle_id) - 1);
            resp.has_head_unit_make = true;
            strncpy(resp.head_unit_make, "custom_ui", sizeof(resp.head_unit_make) - 1);
            resp.has_head_unit_model = true;
            strncpy(resp.head_unit_model, "prado-firmware-reconstruction", sizeof(resp.head_unit_model) - 1);
            resp.has_head_unit_software_build = true;
            strncpy(resp.head_unit_software_build, "1", sizeof(resp.head_unit_software_build) - 1);
            resp.has_head_unit_software_version = true;
            strncpy(resp.head_unit_software_version, "1.0", sizeof(resp.head_unit_software_version) - 1);

            resp.has_display_name = true;
            strncpy(resp.display_name, "custom_ui", sizeof(resp.display_name) - 1);
            resp.has_driver_position = true;
            resp.driver_position = aap_protobuf_service_control_message_DriverPosition_DRIVER_POSITION_LEFT;
            resp.has_probe_for_support = true;
            resp.probe_for_support = false;

            resp.has_headunit_info = true;
            resp.headunit_info.has_make = true;
            strncpy(resp.headunit_info.make, "custom_ui", sizeof(resp.headunit_info.make) - 1);
            resp.headunit_info.has_model = true;
            strncpy(resp.headunit_info.model, "prado-firmware-reconstruction", sizeof(resp.headunit_info.model) - 1);
            resp.headunit_info.has_head_unit_make = true;
            strncpy(resp.headunit_info.head_unit_make, "custom_ui", sizeof(resp.headunit_info.head_unit_make) - 1);
            resp.headunit_info.has_head_unit_model = true;
            strncpy(resp.headunit_info.head_unit_model, "prado-firmware-reconstruction", sizeof(resp.headunit_info.head_unit_model) - 1);

            resp.has_connection_configuration = true;
            resp.connection_configuration.has_ping_configuration = true;
            resp.connection_configuration.ping_configuration.has_tracked_ping_count = true;
            resp.connection_configuration.ping_configuration.tracked_ping_count = 10;
            resp.connection_configuration.ping_configuration.has_timeout_ms = true;
            resp.connection_configuration.ping_configuration.timeout_ms = 10000;
            resp.connection_configuration.ping_configuration.has_interval_ms = true;
            resp.connection_configuration.ping_configuration.interval_ms = 2000;
            resp.connection_configuration.ping_configuration.has_high_latency_threshold_ms = true;
            resp.connection_configuration.ping_configuration.high_latency_threshold_ms = 1000;

            resp.channels_count = 0;

            /* Channel 1: SensorSource */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_SENSOR;
                ch->has_sensor_source_service = true;
                ch->sensor_source_service.sensors_count = 2;
                ch->sensor_source_service.sensors[0].sensor_type = aap_protobuf_service_sensorsource_message_SensorType_SENSOR_DRIVING_STATUS_DATA;
                ch->sensor_source_service.sensors[1].sensor_type = aap_protobuf_service_sensorsource_message_SensorType_SENSOR_NIGHT_MODE;
            }

            /* Channel 3: VideoSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MEDIA_SINK_VIDEO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.has_available_type = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP;
                ch->media_sink_service.has_available_while_in_call = true;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.video_configs_count = 1;
                ch->media_sink_service.video_configs[0].has_codec_resolution = true;
                ch->media_sink_service.video_configs[0].codec_resolution = aap_protobuf_service_media_sink_message_VideoCodecResolutionType_VIDEO_800x480;
                ch->media_sink_service.video_configs[0].has_frame_rate = true;
                ch->media_sink_service.video_configs[0].frame_rate = aap_protobuf_service_media_sink_message_VideoFrameRateType_VIDEO_FPS_30;
                ch->media_sink_service.video_configs[0].has_video_codec_type = true;
                ch->media_sink_service.video_configs[0].video_codec_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP;
                ch->media_sink_service.video_configs[0].has_density = true;
                ch->media_sink_service.video_configs[0].density = 140;
                ch->media_sink_service.video_configs[0].has_real_density = true;
                ch->media_sink_service.video_configs[0].real_density = 140;
                ch->media_sink_service.video_configs[0].has_width_margin = true;
                ch->media_sink_service.video_configs[0].width_margin = 0;
                ch->media_sink_service.video_configs[0].has_height_margin = true;
                ch->media_sink_service.video_configs[0].height_margin = 0;
            }

            /* Channel 4: MediaAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MEDIA_SINK_MEDIA_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.has_available_type = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.has_audio_type = true;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_MEDIA;
                ch->media_sink_service.has_available_while_in_call = true;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 48000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 2;
            }

            /* Channel 5: GuidanceAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MEDIA_SINK_GUIDANCE_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.has_available_type = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.has_audio_type = true;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_GUIDANCE;
                ch->media_sink_service.has_available_while_in_call = true;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 16000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 1;
            }

            /* Channel 6: SystemAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MEDIA_SINK_SYSTEM_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.has_available_type = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.has_audio_type = true;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_SYSTEM_AUDIO;
                ch->media_sink_service.has_available_while_in_call = true;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 16000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 1;
            }

            /* Channel 8: InputSource */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_INPUT;
                ch->has_input_source_service = true;
                ch->input_source_service.touchscreen_count = 1;
                ch->input_source_service.touchscreen[0].width = 800;
                ch->input_source_service.touchscreen[0].height = 480;
                ch->input_source_service.touchscreen[0].has_type = true;
                ch->input_source_service.touchscreen[0].type = aap_protobuf_service_inputsource_message_TouchScreenType_CAPACITIVE;
                ch->input_source_service.keycodes_supported_count = 8;
                ch->input_source_service.keycodes_supported[0] = 19; /* KEYCODE_DPAD_UP */
                ch->input_source_service.keycodes_supported[1] = 20; /* KEYCODE_DPAD_DOWN */
                ch->input_source_service.keycodes_supported[2] = 21; /* KEYCODE_DPAD_LEFT */
                ch->input_source_service.keycodes_supported[3] = 22; /* KEYCODE_DPAD_RIGHT */
                ch->input_source_service.keycodes_supported[4] = 23; /* KEYCODE_DPAD_CENTER */
                ch->input_source_service.keycodes_supported[5] = 3;  /* KEYCODE_HOME */
                ch->input_source_service.keycodes_supported[6] = 65538; /* KEYCODE_NAVIGATION */
                ch->input_source_service.keycodes_supported[7] = 87; /* KEYCODE_MEDIA_NEXT */
            }

            /* Channel 9: Microphone */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MICROPHONE;
                ch->has_media_source_service = true;
                ch->media_source_service.has_available_type = true;
                ch->media_source_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_source_service.has_audio_config = true;
                ch->media_source_service.audio_config.sampling_rate = 16000;
                ch->media_source_service.audio_config.number_of_bits = 16;
                ch->media_source_service.audio_config.number_of_channels = 1;
            }

            /* Channel 10: Bluetooth */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_BLUETOOTH;
                ch->has_bluetooth_service = true;
                strncpy(ch->bluetooth_service.car_address, "", sizeof(ch->bluetooth_service.car_address) - 1);
                ch->bluetooth_service.supported_pairing_methods_count = 1;
                ch->bluetooth_service.supported_pairing_methods[0] = aap_protobuf_service_bluetooth_message_BluetoothPairingMethod_BLUETOOTH_PAIRING_UNAVAILABLE;
            }

            uint8_t pb_buf[4096];
            pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            bool enc_ok = pb_encode(&stream, aap_protobuf_service_control_message_ServiceDiscoveryResponse_fields, &resp);
            if (!enc_ok) {
                fprintf(stderr, "[AA] ERROR: ServiceDiscoveryResponse encoding failed: %s\n", PB_GET_ERROR(&stream));
            } else {
                printf("[AA] ServiceDiscoveryResponse encoded (%zu bytes, success=1)\n", stream.bytes_written);
            }

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_SERVICE_DISCOVERY_RESPONSE,
                             pb_buf, stream.bytes_written, true);
            set_state(s, AAP_SESSION_STATE_CHANNELS_OPENING, "Service discovery complete, opening channels");

            /* Send immediate initial keepalive ping */
            aap_protobuf_service_control_message_PingRequest ping =
                aap_protobuf_service_control_message_PingRequest_init_default;
            ping.timestamp = plausible_epoch_millis();
            stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&stream, aap_protobuf_service_control_message_PingRequest_fields, &ping);
            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_PING_REQUEST,
                             pb_buf, stream.bytes_written, true);
            s->last_ping_time = time(NULL);
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_PING_REQUEST: {
            aap_protobuf_service_control_message_PingRequest req =
                aap_protobuf_service_control_message_PingRequest_init_default;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
            pb_decode(&stream, aap_protobuf_service_control_message_PingRequest_fields, &req);

            aap_protobuf_service_control_message_PingResponse resp =
                aap_protobuf_service_control_message_PingResponse_init_default;
            resp.timestamp = req.timestamp;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_PingResponse_fields, &resp);

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_PING_RESPONSE,
                             pb_buf, ostream.bytes_written, true);
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_PING_RESPONSE: {
            /* Ping reply received from phone */
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_AUDIO_FOCUS_REQUEST: {
            aap_protobuf_service_control_message_AudioFocusRequest req =
                aap_protobuf_service_control_message_AudioFocusRequest_init_default;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
            pb_decode(&stream, aap_protobuf_service_control_message_AudioFocusRequest_fields, &req);

            printf("[AA] received AudioFocusRequest (type=%d)\n", req.audio_focus_type);

            aap_protobuf_service_control_message_AudioFocusNotification resp =
                aap_protobuf_service_control_message_AudioFocusNotification_init_default;
            if (req.audio_focus_type == aap_protobuf_service_control_message_AudioFocusRequestType_AUDIO_FOCUS_GAIN) {
                resp.focus_state = aap_protobuf_service_control_message_AudioFocusStateType_AUDIO_FOCUS_STATE_GAIN;
            } else if (req.audio_focus_type == aap_protobuf_service_control_message_AudioFocusRequestType_AUDIO_FOCUS_GAIN_TRANSIENT ||
                       req.audio_focus_type == aap_protobuf_service_control_message_AudioFocusRequestType_AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK) {
                resp.focus_state = aap_protobuf_service_control_message_AudioFocusStateType_AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
            } else {
                resp.focus_state = aap_protobuf_service_control_message_AudioFocusStateType_AUDIO_FOCUS_STATE_LOSS;
            }
            resp.has_unsolicited = true;
            resp.unsolicited = false;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_AudioFocusNotification_fields, &resp);

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_AUDIO_FOCUS_NOTIFICATION,
                             pb_buf, ostream.bytes_written, true);
            printf("[AA] audio focus response sent (state=%d)\n", resp.focus_state);
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_NAV_FOCUS_REQUEST: {
            printf("[AA] received NavFocusRequest\n");
            aap_protobuf_service_control_message_NavFocusNotification resp =
                aap_protobuf_service_control_message_NavFocusNotification_init_default;
            resp.focus_type = aap_protobuf_service_control_message_NavFocusType_NAV_FOCUS_PROJECTED;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_NavFocusNotification_fields, &resp);

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_NAV_FOCUS_NOTIFICATION,
                             pb_buf, ostream.bytes_written, true);
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_VOICE_SESSION_NOTIFICATION: {
            aap_protobuf_service_control_message_VoiceSessionNotification notif =
                aap_protobuf_service_control_message_VoiceSessionNotification_init_default;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
            pb_decode(&stream, aap_protobuf_service_control_message_VoiceSessionNotification_fields, &notif);

            printf("[AA] received VoiceSessionNotification (status=%d)\n", notif.status);

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_VoiceSessionNotification_fields, &notif);

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_VOICE_SESSION_NOTIFICATION,
                             pb_buf, ostream.bytes_written, true);
            break;
        }

        case aap_protobuf_service_control_message_ControlMessageType_MESSAGE_BYEBYE_REQUEST: {
            printf("[AA] received ByeByeRequest\n");
            aap_protobuf_service_control_message_ByeByeResponse resp =
                aap_protobuf_service_control_message_ByeByeResponse_init_default;

            uint8_t pb_buf[64];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_ByeByeResponse_fields, &resp);

            send_channel_msg(s, AAP_CHANNEL_CONTROL,
                             aap_protobuf_service_control_message_ControlMessageType_MESSAGE_BYEBYE_RESPONSE,
                             pb_buf, ostream.bytes_written, true);
            set_state(s, AAP_SESSION_STATE_DISCONNECTED, "Phone requested disconnect (ByeBye)");
            break;
        }

        default:
            printf("[AA] unhandled control message 0x%04X\n", msg_id);
            break;
    }
}

static void handle_sensor_channel(aap_session_t *s, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    if (sub_cmd == aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_REQUEST) {
        handle_channel_open_request(s, AAP_CHANNEL_SENSOR, pb_data, pb_len);
        return;
    }

    if (sub_cmd == aap_protobuf_service_sensorsource_SensorMessageId_SENSOR_MESSAGE_REQUEST) {
        aap_protobuf_service_sensorsource_message_SensorRequest req =
            aap_protobuf_service_sensorsource_message_SensorRequest_init_default;
        pb_istream_t istream = pb_istream_from_buffer(pb_data, pb_len);
        pb_decode(&istream, aap_protobuf_service_sensorsource_message_SensorRequest_fields, &req);

        printf("[AA] sensor start request (type=%d)\n", req.type);

        aap_protobuf_service_sensorsource_message_SensorStartResponseMessage resp =
            aap_protobuf_service_sensorsource_message_SensorStartResponseMessage_init_default;
        resp.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;

        uint8_t pb_buf[256];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_sensorsource_message_SensorStartResponseMessage_fields, &resp);

        send_channel_msg(s, AAP_CHANNEL_SENSOR,
                         aap_protobuf_service_sensorsource_SensorMessageId_SENSOR_MESSAGE_RESPONSE,
                         pb_buf, ostream.bytes_written, true);
        printf("[AA] sensor start response sent (type=%d, STATUS_SUCCESS)\n", req.type);

        /* Send initial sensor batch data */
        aap_protobuf_service_sensorsource_message_SensorBatch batch =
            aap_protobuf_service_sensorsource_message_SensorBatch_init_default;
        if (req.type == aap_protobuf_service_sensorsource_message_SensorType_SENSOR_DRIVING_STATUS_DATA) {
            batch.driving_status_data_count = 1;
            batch.driving_status_data[0].status = 0; /* Unrestricted */
        } else if (req.type == aap_protobuf_service_sensorsource_message_SensorType_SENSOR_NIGHT_MODE) {
            batch.night_mode_data_count = 1;
            batch.night_mode_data[0].has_night_mode = true;
            batch.night_mode_data[0].night_mode = false;
        }

        ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        bool ok = pb_encode(&ostream, aap_protobuf_service_sensorsource_message_SensorBatch_fields, &batch);
        send_channel_msg(s, AAP_CHANNEL_SENSOR,
                         aap_protobuf_service_sensorsource_SensorMessageId_SENSOR_MESSAGE_BATCH,
                         pb_buf, ostream.bytes_written, true);
        printf("[AA] sensor batch data sent (type=%d, ok=%d, bytes=%zu)\n", req.type, ok, ostream.bytes_written);
    }
}

static void handle_input_channel(aap_session_t *s, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    if (sub_cmd == aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_REQUEST) {
        handle_channel_open_request(s, AAP_CHANNEL_INPUT, pb_data, pb_len);
        return;
    }

    if (sub_cmd == aap_protobuf_service_inputsource_InputMessageId_INPUT_MESSAGE_KEY_BINDING_REQUEST) {
        printf("[AA] input KeyBindingRequest received -> replying STATUS_SUCCESS\n");
        aap_protobuf_service_media_sink_message_KeyBindingResponse resp =
            aap_protobuf_service_media_sink_message_KeyBindingResponse_init_default;
        resp.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_media_sink_message_KeyBindingResponse_fields, &resp);

        send_channel_msg(s, AAP_CHANNEL_INPUT,
                         aap_protobuf_service_inputsource_InputMessageId_INPUT_MESSAGE_KEY_BINDING_RESPONSE,
                         pb_buf, ostream.bytes_written, true);
    }
}

static void on_mic_pcm_captured(aap_session_t *s, uint64_t timestamp_us, const uint8_t *data, size_t len) {
    if (!s || s->socket_fd < 0 || !s->cryptor) return;

    uint8_t raw_msg[512];
    if (len + 10 > sizeof(raw_msg)) return;

    /* Message ID 0x0000 (MEDIA_DATA) */
    raw_msg[0] = 0x00;
    raw_msg[1] = 0x00;

    /* 8-byte big-endian timestamp in microseconds */
    raw_msg[2] = (uint8_t)((timestamp_us >> 56) & 0xFF);
    raw_msg[3] = (uint8_t)((timestamp_us >> 48) & 0xFF);
    raw_msg[4] = (uint8_t)((timestamp_us >> 40) & 0xFF);
    raw_msg[5] = (uint8_t)((timestamp_us >> 32) & 0xFF);
    raw_msg[6] = (uint8_t)((timestamp_us >> 24) & 0xFF);
    raw_msg[7] = (uint8_t)((timestamp_us >> 16) & 0xFF);
    raw_msg[8] = (uint8_t)((timestamp_us >> 8) & 0xFF);
    raw_msg[9] = (uint8_t)(timestamp_us & 0xFF);

    memcpy(&raw_msg[10], data, len);
    size_t total_len = len + 10;

    uint8_t enc_buf[512 + 64];
    size_t enc_len = aap_cryptor_encrypt(s->cryptor, raw_msg, total_len, enc_buf, sizeof(enc_buf));
    if (enc_len > 0) {
        send_raw_frame(s, AAP_CHANNEL_MICROPHONE, AAP_FRAME_BULK, false, true, enc_buf, enc_len);
    }
}

static void handle_microphone_channel(aap_session_t *s, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    if (sub_cmd == aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_REQUEST) {
        handle_channel_open_request(s, AAP_CHANNEL_MICROPHONE, pb_data, pb_len);
        return;
    }

    if (sub_cmd == aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_SETUP) {
        printf("[AA] microphone channel setup request\n");
        aap_protobuf_service_media_shared_message_Config cfg =
            aap_protobuf_service_media_shared_message_Config_init_default;
        cfg.status = aap_protobuf_service_media_shared_message_Config_Status_STATUS_READY;
        cfg.has_max_unacked = true;
        cfg.max_unacked = 1;
        cfg.configuration_indices_count = 1;
        cfg.configuration_indices[0] = 0;

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_media_shared_message_Config_fields, &cfg);

        send_channel_msg(s, AAP_CHANNEL_MICROPHONE, aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_CONFIG,
                         pb_buf, ostream.bytes_written, true);
        printf("[AA] microphone channel setup response sent\n");
    } else if (sub_cmd == 32769 /* MicrophoneRequest */) {
        aap_protobuf_service_media_source_message_MicrophoneRequest req =
            aap_protobuf_service_media_source_message_MicrophoneRequest_init_default;
        if (pb_len > 0) {
            pb_istream_t istream = pb_istream_from_buffer(pb_data, pb_len);
            pb_decode(&istream, aap_protobuf_service_media_source_message_MicrophoneRequest_fields, &req);
        }

        printf("[AA] microphone request (open=%d)\n", req.open);

        aap_protobuf_service_media_source_message_MicrophoneResponse resp =
            aap_protobuf_service_media_source_message_MicrophoneResponse_init_default;
        resp.status = (int32_t)aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;
        resp.has_session_id = true;
        resp.session_id = 0;

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_media_source_message_MicrophoneResponse_fields, &resp);

        send_channel_msg(s, AAP_CHANNEL_MICROPHONE, 32770 /* MicrophoneResponse */,
                         pb_buf, ostream.bytes_written, true);
        printf("[AA] microphone open/close response sent\n");

        if (req.open) {
            aap_microphone_start(s->mic, s, on_mic_pcm_captured);
        } else {
            aap_microphone_stop(s->mic);
        }
    }
}

static void handle_bluetooth_channel(aap_session_t *s, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    if (sub_cmd == aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_REQUEST) {
        handle_channel_open_request(s, AAP_CHANNEL_BLUETOOTH, pb_data, pb_len);
        return;
    }

    if (sub_cmd == aap_protobuf_service_bluetooth_BluetoothMessageId_BLUETOOTH_MESSAGE_PAIRING_REQUEST) {
        aap_protobuf_service_bluetooth_message_BluetoothPairingRequest req =
            aap_protobuf_service_bluetooth_message_BluetoothPairingRequest_init_default;
        if (pb_len > 0) {
            pb_istream_t stream = pb_istream_from_buffer(pb_data, pb_len);
            pb_decode(&stream, aap_protobuf_service_bluetooth_message_BluetoothPairingRequest_fields, &req);
        }

        printf("[AA] bluetooth pairing request from '%s'\n", req.phone_address);

        aap_protobuf_service_bluetooth_message_BluetoothPairingResponse resp =
            aap_protobuf_service_bluetooth_message_BluetoothPairingResponse_init_default;
        resp.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;
        resp.already_paired = true;

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_bluetooth_message_BluetoothPairingResponse_fields, &resp);

        send_channel_msg(s, AAP_CHANNEL_BLUETOOTH,
                         aap_protobuf_service_bluetooth_BluetoothMessageId_BLUETOOTH_MESSAGE_PAIRING_RESPONSE,
                         pb_buf, ostream.bytes_written, true);
        printf("[AA] bluetooth pairing response sent (STATUS_SUCCESS, already_paired=true)\n");
    } else if (sub_cmd == aap_protobuf_service_bluetooth_BluetoothMessageId_BLUETOOTH_MESSAGE_AUTHENTICATION_RESULT) {
        printf("[AA] bluetooth authentication result received\n");
    }
}

static void handle_media_channel(aap_session_t *s, uint8_t channel_id, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    if (sub_cmd == aap_protobuf_service_control_message_ControlMessageType_MESSAGE_CHANNEL_OPEN_REQUEST) {
        handle_channel_open_request(s, channel_id, pb_data, pb_len);
        return;
    }

    switch (sub_cmd) {
        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_SETUP: {
            printf("[AA] channel %u (%s) setup request\n", channel_id, channel_name(channel_id));
            aap_protobuf_service_media_shared_message_Config cfg =
                aap_protobuf_service_media_shared_message_Config_init_default;
            cfg.status = aap_protobuf_service_media_shared_message_Config_Status_STATUS_READY;
            cfg.has_max_unacked = true;
            cfg.max_unacked = (channel_id == AAP_CHANNEL_MEDIA_SINK_VIDEO) ? 1 : 8;
            cfg.configuration_indices_count = 1;
            cfg.configuration_indices[0] = 0;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_media_shared_message_Config_fields, &cfg);

            send_channel_msg(s, channel_id, aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_CONFIG,
                             pb_buf, ostream.bytes_written, true);
            printf("[AA] channel %u (%s) setup response sent\n", channel_id, channel_name(channel_id));

            /* If this is the video sink, send unsolicited VideoFocusNotification (PROJECTED) */
            if (channel_id == AAP_CHANNEL_MEDIA_SINK_VIDEO) {
                aap_protobuf_service_media_video_message_VideoFocusNotification focus_notif =
                    aap_protobuf_service_media_video_message_VideoFocusNotification_init_default;
                focus_notif.has_focus = true;
                focus_notif.focus = aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_PROJECTED;
                focus_notif.has_unsolicited = true;
                focus_notif.unsolicited = true;

                ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
                pb_encode(&ostream, aap_protobuf_service_media_video_message_VideoFocusNotification_fields, &focus_notif);
                send_channel_msg(s, AAP_CHANNEL_MEDIA_SINK_VIDEO,
                                 aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION,
                                 pb_buf, ostream.bytes_written, true);
                printf("[AA] video focus indication sent (unsolicited=1)\n");
            }
            break;
        }

        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_VIDEO_FOCUS_REQUEST: {
            printf("[AA] video focus request received\n");
            aap_protobuf_service_media_video_message_VideoFocusRequestNotification req =
                aap_protobuf_service_media_video_message_VideoFocusRequestNotification_init_default;
            if (pb_len > 0) {
                pb_istream_t istream = pb_istream_from_buffer(pb_data, pb_len);
                pb_decode(&istream, aap_protobuf_service_media_video_message_VideoFocusRequestNotification_fields, &req);
            }

            aap_protobuf_service_media_video_message_VideoFocusNotification focus_notif =
                aap_protobuf_service_media_video_message_VideoFocusNotification_init_default;
            focus_notif.has_focus = true;
            focus_notif.focus = req.has_mode ? req.mode : aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_PROJECTED;
            focus_notif.has_unsolicited = true;
            focus_notif.unsolicited = false;

            s->is_video_focus_native = (focus_notif.focus == aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_NATIVE ||
                                        focus_notif.focus == aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_NATIVE_TRANSIENT);
            aap_video_sink_set_visible(s->video_sink, !s->is_video_focus_native);

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_media_video_message_VideoFocusNotification_fields, &focus_notif);
            send_channel_msg(s, AAP_CHANNEL_MEDIA_SINK_VIDEO,
                             aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION,
                             pb_buf, ostream.bytes_written, true);
            printf("[AA] video focus indication sent (unsolicited=0, mode=%d, native=%d)\n",
                   focus_notif.focus, s->is_video_focus_native);
            break;
        }

        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_START: {
            aap_protobuf_service_media_shared_message_Start start =
                aap_protobuf_service_media_shared_message_Start_init_default;
            pb_istream_t istream = pb_istream_from_buffer(pb_data, pb_len);
            pb_decode(&istream, aap_protobuf_service_media_shared_message_Start_fields, &start);

            printf("[AA] channel %u (%s) start (session_id=%d)\n", channel_id, channel_name(channel_id), start.session_id);
            if (channel_id == AAP_CHANNEL_MEDIA_SINK_MEDIA_AUDIO) {
                s->media_session_id = start.session_id;
                aap_audio_sink_open(s->audio_media);
                aap_audio_sink_prepare(s->audio_media);
            } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_GUIDANCE_AUDIO) {
                aap_audio_sink_open(s->audio_guidance);
            } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_SYSTEM_AUDIO) {
                aap_audio_sink_open(s->audio_system);
            } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_VIDEO) {
                s->video_session_id = start.session_id;
                aap_video_sink_open(s->video_sink);
                set_state(s, AAP_SESSION_STATE_RUNNING, "Android Auto session running (video active)");
            }
            break;
        }

        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_STOP: {
            printf("[AA] channel %u (%s) stop\n", channel_id, channel_name(channel_id));
            if (channel_id == AAP_CHANNEL_MEDIA_SINK_MEDIA_AUDIO) {
                aap_audio_sink_flush(s->audio_media);
            }
            break;
        }

        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_CODEC_CONFIG: {
            /* SPS/PPS NAL units without timestamp, offset 2 */
            if (payload_len > 2 && channel_id == AAP_CHANNEL_MEDIA_SINK_VIDEO) {
                aap_video_sink_decode(s->video_sink, payload + 2, payload_len - 2);
            }
            break;
        }

        case aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_DATA: {
            /* Timestamp is 8 bytes at payload + 2, media stream starts at payload + 10 */
            if (payload_len > 10) {
                const uint8_t *stream_bytes = payload + 10;
                size_t stream_bytes_len = payload_len - 10;

                if (channel_id == AAP_CHANNEL_MEDIA_SINK_MEDIA_AUDIO) {
                    aap_audio_sink_write(s->audio_media, stream_bytes, stream_bytes_len);
                    send_media_ack(s, channel_id, s->media_session_id, 1);
                } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_GUIDANCE_AUDIO) {
                    aap_audio_sink_write(s->audio_guidance, stream_bytes, stream_bytes_len);
                    send_media_ack(s, channel_id, s->media_session_id, 1);
                } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_SYSTEM_AUDIO) {
                    aap_audio_sink_write(s->audio_system, stream_bytes, stream_bytes_len);
                    send_media_ack(s, channel_id, s->media_session_id, 1);
                } else if (channel_id == AAP_CHANNEL_MEDIA_SINK_VIDEO) {
                    aap_video_sink_decode(s->video_sink, stream_bytes, stream_bytes_len);
                    send_media_ack(s, channel_id, s->video_session_id, 1);
                }
            }
            break;
        }

        default: {
            printf("[AA] channel %u (%s) unhandled sub_cmd 0x%04X (len=%zu)\n",
                   channel_id, channel_name(channel_id), sub_cmd, payload_len);
            break;
        }
    }
}
aap_session_t *aap_session_create(int tcp_fd) {
    if (tcp_fd < 0) return NULL;

    aap_session_t *s = (aap_session_t *)calloc(1, sizeof(aap_session_t));
    if (!s) return NULL;

    s->socket_fd = tcp_fd;
    s->cryptor = aap_cryptor_create();
    s->audio_media = aap_audio_sink_create("plug:softvol2", 48000, 2);
    s->audio_guidance = aap_audio_sink_create("plug:softvol1", 16000, 1);
    s->audio_system = aap_audio_sink_create("plug:softvol4", 16000, 1);
    s->video_sink = aap_video_sink_create(800, 480);
    s->mic = aap_microphone_create("default");

    s->last_ping_time = time(NULL);
    s->last_rx_time = time(NULL);

    send_version_request(s);
    return s;
}

void aap_session_destroy(aap_session_t *s) {
    if (!s) return;
    if (s->socket_fd >= 0) {
        close(s->socket_fd);
        s->socket_fd = -1;
    }
    for (int i = 0; i < AAP_MAX_CHANNELS; i++) {
        if (s->assemblers[i].buf) {
            free(s->assemblers[i].buf);
            s->assemblers[i].buf = NULL;
        }
    }
    aap_cryptor_destroy(s->cryptor);
    aap_audio_sink_destroy(s->audio_media);
    aap_audio_sink_destroy(s->audio_guidance);
    aap_audio_sink_destroy(s->audio_system);
    aap_video_sink_destroy(s->video_sink);
    if (s->mic) {
        aap_microphone_destroy(s->mic);
        s->mic = NULL;
    }
    free(s);
}

aap_session_state_t aap_session_get_state(const aap_session_t *session) {
    return session ? session->state : AAP_SESSION_STATE_IDLE;
}

const char *aap_session_get_status_message(const aap_session_t *session) {
    return session ? session->status_message : "";
}

int aap_session_get_socket_fd(const aap_session_t *session) {
    return session ? session->socket_fd : -1;
}

static void dispatch_aap_message(aap_session_t *s, uint8_t channel_id, uint8_t flags, const uint8_t *payload, size_t len) {
    (void)flags;
    if (channel_id == AAP_CHANNEL_CONTROL) {
        if (len >= 2) {
            uint16_t msg_id = (uint16_t)((payload[0] << 8) | payload[1]);
            handle_control_message(s, msg_id, payload + 2, len - 2);
        }
    } else if (channel_id == AAP_CHANNEL_SENSOR) {
        handle_sensor_channel(s, payload, len);
    } else if (channel_id == AAP_CHANNEL_INPUT) {
        handle_input_channel(s, payload, len);
    } else if (channel_id == AAP_CHANNEL_MICROPHONE) {
        handle_microphone_channel(s, payload, len);
    } else if (channel_id == AAP_CHANNEL_BLUETOOTH) {
        handle_bluetooth_channel(s, payload, len);
    } else {
        handle_media_channel(s, channel_id, payload, len);
    }
}

bool aap_session_process_incoming(aap_session_t *s) {
    if (!s || s->socket_fd < 0) return false;

    ssize_t n = read(s->socket_fd, s->rx_buf + s->rx_len, RX_BUFFER_SIZE - s->rx_len);
    if (n <= 0) {
        set_state(s, AAP_SESSION_STATE_DISCONNECTED, "Socket closed by peer");
        return false;
    }

    s->rx_len += (size_t)n;
    s->last_rx_time = time(NULL);

    size_t cursor = 0;
    while (s->rx_len - cursor >= 2) {
        aap_frame_header_t hdr;
        size_t hdr_len = aap_parse_frame_header(s->rx_buf + cursor, s->rx_len - cursor, &hdr);
        if (hdr_len == 0) {
            break; /* Incomplete header */
        }

        size_t frame_total = hdr_len + hdr.frame_size;
        if (s->rx_len - cursor < frame_total) {
            break; /* Incomplete payload */
        }

        const uint8_t *frame_payload = s->rx_buf + cursor + hdr_len;
        size_t payload_len = hdr.frame_size;

        uint8_t plain_buf[AAP_MAX_PAYLOAD_SIZE + 64];
        const uint8_t *active_payload = frame_payload;
        size_t active_len = payload_len;

        if (hdr.flags & AAP_FLAG_ENCRYPTED) {
            size_t dec_len = aap_cryptor_decrypt(s->cryptor, frame_payload, payload_len, plain_buf, sizeof(plain_buf));
            if (dec_len > 0) {
                active_payload = plain_buf;
                active_len = dec_len;
            } else {
                fprintf(stderr, "[AA] decrypt failed on channel %u (%s)\n", hdr.channel_id, channel_name(hdr.channel_id));
                cursor += frame_total;
                continue;
            }
        }

        uint8_t frame_type = hdr.flags & AAP_FLAG_FRAME_TYPE_MASK;
        uint8_t ch = hdr.channel_id;

        if (frame_type == AAP_FRAME_BULK) {
            dispatch_aap_message(s, ch, hdr.flags, active_payload, active_len);
        } else if (ch < AAP_MAX_CHANNELS) {
            aap_channel_assembler_t *asm_buf = &s->assemblers[ch];

            if (frame_type == AAP_FRAME_FIRST) {
                asm_buf->len = 0;
                asm_buf->flags = hdr.flags;
                size_t needed = hdr.total_size > 0 ? (size_t)hdr.total_size : (active_len + 8192);
                if (asm_buf->capacity < needed) {
                    uint8_t *new_buf = (uint8_t *)realloc(asm_buf->buf, needed);
                    if (new_buf) {
                        asm_buf->buf = new_buf;
                        asm_buf->capacity = needed;
                    }
                }
            }

            if (!asm_buf->buf) {
                size_t init_cap = active_len + 8192;
                asm_buf->buf = (uint8_t *)malloc(init_cap);
                if (asm_buf->buf) {
                    asm_buf->capacity = init_cap;
                    asm_buf->len = 0;
                }
            }

            if (asm_buf->buf) {
                if (asm_buf->len + active_len > asm_buf->capacity) {
                    size_t new_cap = asm_buf->len + active_len + 16384;
                    uint8_t *new_buf = (uint8_t *)realloc(asm_buf->buf, new_cap);
                    if (new_buf) {
                        asm_buf->buf = new_buf;
                        asm_buf->capacity = new_cap;
                    }
                }
                if (asm_buf->len + active_len <= asm_buf->capacity) {
                    memcpy(asm_buf->buf + asm_buf->len, active_payload, active_len);
                    asm_buf->len += active_len;
                }
            }

            if (frame_type == AAP_FRAME_LAST) {
                if (asm_buf->buf && asm_buf->len > 0) {
                    dispatch_aap_message(s, ch, asm_buf->flags, asm_buf->buf, asm_buf->len);
                    asm_buf->len = 0;
                }
            }
        }

        cursor += frame_total;
    }

    if (cursor > 0) {
        if (cursor < s->rx_len) {
            memmove(s->rx_buf, s->rx_buf + cursor, s->rx_len - cursor);
        }
        s->rx_len -= cursor;
    }

    return true;
}

void aap_session_tick(aap_session_t *s) {
    if (!s || (s->state != AAP_SESSION_STATE_RUNNING && s->state != AAP_SESSION_STATE_CHANNELS_OPENING)) return;

    time_t now = time(NULL);
    if (s->state == AAP_SESSION_STATE_RUNNING && (now - s->last_rx_time > 3)) {
        printf("aap_session: WiFi connection lost (no data for 3s), closing session\n");
        set_state(s, AAP_SESSION_STATE_DISCONNECTED, "WiFi disconnected");
        return;
    }

    if (now - s->last_ping_time >= 1) {
        s->last_ping_time = now;

        aap_protobuf_service_control_message_PingRequest ping =
            aap_protobuf_service_control_message_PingRequest_init_default;
        ping.timestamp = plausible_epoch_millis();

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_control_message_PingRequest_fields, &ping);

        send_channel_msg(s, AAP_CHANNEL_CONTROL,
                         aap_protobuf_service_control_message_ControlMessageType_MESSAGE_PING_REQUEST,
                         pb_buf, ostream.bytes_written, true);
    }
}

void aap_session_send_key(aap_session_t *s, uint32_t keycode) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    /* Key down report */
    aap_protobuf_service_inputsource_message_InputReport report =
        aap_protobuf_service_inputsource_message_InputReport_init_default;
    report.timestamp = now_micros();
    report.has_key_event = true;
    report.key_event.keys_count = 1;
    report.key_event.keys[0].keycode = keycode;
    report.key_event.keys[0].down = true;
    report.key_event.keys[0].metastate = 0;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_inputsource_message_InputReport_fields, &report);

    send_channel_msg(s, AAP_CHANNEL_INPUT,
                     aap_protobuf_service_inputsource_InputMessageId_INPUT_MESSAGE_INPUT_REPORT,
                     pb_buf, ostream.bytes_written, true);

    /* 25ms key press hold duration so Android InputManager receives distinct down and up events */
    usleep(25000);

    /* Key up report */
    report.timestamp = now_micros();
    report.key_event.keys[0].down = false;

    ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_inputsource_message_InputReport_fields, &report);

    send_channel_msg(s, AAP_CHANNEL_INPUT,
                     aap_protobuf_service_inputsource_InputMessageId_INPUT_MESSAGE_INPUT_REPORT,
                     pb_buf, ostream.bytes_written, true);

    printf("aap_session: sent keycode %u (down/up)\n", keycode);
}

void aap_session_send_touch(aap_session_t *s, uint32_t x, uint32_t y, uint32_t action) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    aap_protobuf_service_inputsource_message_InputReport report =
        aap_protobuf_service_inputsource_message_InputReport_init_default;
    report.timestamp = now_micros();
    report.has_touch_event = true;
    report.touch_event.has_action = true;
    report.touch_event.action = (aap_protobuf_service_inputsource_message_PointerAction)action;
    report.touch_event.pointer_data_count = 1;
    report.touch_event.pointer_data[0].x = x;
    report.touch_event.pointer_data[0].y = y;
    report.touch_event.pointer_data[0].pointer_id = 0;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_inputsource_message_InputReport_fields, &report);

    send_channel_msg(s, AAP_CHANNEL_INPUT,
                     aap_protobuf_service_inputsource_InputMessageId_INPUT_MESSAGE_INPUT_REPORT,
                     pb_buf, ostream.bytes_written, true);
}

void aap_session_send_night_mode(aap_session_t *s, bool night_mode) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    aap_protobuf_service_sensorsource_message_SensorBatch batch =
        aap_protobuf_service_sensorsource_message_SensorBatch_init_default;
    batch.night_mode_data_count = 1;
    batch.night_mode_data[0].has_night_mode = true;
    batch.night_mode_data[0].night_mode = night_mode;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_sensorsource_message_SensorBatch_fields, &batch);

    send_channel_msg(s, AAP_CHANNEL_SENSOR,
                     aap_protobuf_service_sensorsource_SensorMessageId_SENSOR_MESSAGE_BATCH,
                     pb_buf, ostream.bytes_written, true);
}

void aap_session_set_video_visible(aap_session_t *s, bool visible) {
    if (!s || !s->video_sink) return;
    aap_video_sink_set_visible(s->video_sink, visible);
}

bool aap_session_is_video_focus_native(const aap_session_t *s) {
    return s ? s->is_video_focus_native : false;
}

void aap_session_request_video_focus(aap_session_t *s, bool projected) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    aap_protobuf_service_media_video_message_VideoFocusNotification focus_notif =
        aap_protobuf_service_media_video_message_VideoFocusNotification_init_default;
    focus_notif.has_focus = true;
    focus_notif.focus = projected ? aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_PROJECTED
                                  : aap_protobuf_service_media_video_message_VideoFocusMode_VIDEO_FOCUS_NATIVE;
    focus_notif.has_unsolicited = true;
    focus_notif.unsolicited = true;

    uint8_t pb_buf[128];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_media_video_message_VideoFocusNotification_fields, &focus_notif);
    send_channel_msg(s, AAP_CHANNEL_MEDIA_SINK_VIDEO,
                     aap_protobuf_service_media_sink_MediaMessageId_MEDIA_MESSAGE_VIDEO_FOCUS_NOTIFICATION,
                     pb_buf, ostream.bytes_written, true);

    s->is_video_focus_native = !projected;
    aap_video_sink_set_visible(s->video_sink, projected);
    printf("aap_session: video focus request sent (unsolicited=1, projected=%d)\n", projected ? 1 : 0);
}
