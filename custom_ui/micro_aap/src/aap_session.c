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

#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/control/message/ByeByeRequest.pb.h"
#include "aap_protobuf/service/control/message/ByeByeResponse.pb.h"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"

#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/shared/message/Stop.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"

#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"

#define AAP_MSG_VERSION_REQUEST           0x0001
#define AAP_MSG_VERSION_RESPONSE          0x0002
#define AAP_MSG_SSL_HANDSHAKE             0x0003
#define AAP_MSG_SERVICE_DISCOVERY_REQUEST 0x0004
#define AAP_MSG_SERVICE_DISCOVERY_RESPONSE 0x0005
#define AAP_MSG_AUTH_COMPLETE             0x0006
#define AAP_MSG_CHANNEL_OPEN_REQUEST      0x0007
#define AAP_MSG_CHANNEL_OPEN_RESPONSE     0x0008
#define AAP_MSG_PING_REQUEST              0x000B
#define AAP_MSG_PING_RESPONSE             0x000C
#define AAP_MSG_BYE_BYE_REQUEST           0x000D
#define AAP_MSG_BYE_BYE_RESPONSE          0x000E

#define AAP_MEDIA_MSG_SETUP_REQ           0x8000
#define AAP_MEDIA_MSG_START_IND           0x8001
#define AAP_MEDIA_MSG_STOP_IND            0x8002
#define AAP_MEDIA_MSG_SETUP_RESP          0x8003
#define AAP_MEDIA_MSG_ACK_IND             0x8004

#define RX_BUFFER_SIZE (64 * 1024)
#define TX_BUFFER_SIZE (64 * 1024)

struct aap_session {
    int socket_fd;
    aap_session_state_t state;
    char status_message[128];

    aap_cryptor_t *cryptor;
    aap_audio_sink_t *audio_media;
    aap_audio_sink_t *audio_guidance;
    aap_audio_sink_t *audio_system;
    aap_video_sink_t *video_sink;

    uint8_t rx_buf[RX_BUFFER_SIZE];
    size_t rx_len;

    uint8_t tx_buf[TX_BUFFER_SIZE];

    int32_t media_session_id;
    int32_t video_session_id;

    time_t last_ping_time;
    time_t last_rx_time;
};

static void set_state(aap_session_t *s, aap_session_state_t state, const char *msg) {
    s->state = state;
    strncpy(s->status_message, msg ? msg : "", sizeof(s->status_message) - 1);
    printf("aap_session: [%d] %s\n", state, s->status_message);
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

static bool send_control_msg(aap_session_t *s, uint16_t msg_id, const uint8_t *proto_data, size_t proto_len, bool encrypt) {
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
        return send_raw_frame(s, AAP_CHANNEL_CONTROL, AAP_FRAME_BULK, true, true, cipher, cipher_len);
    } else {
        return send_raw_frame(s, AAP_CHANNEL_CONTROL, AAP_FRAME_BULK, true, false, raw_msg, total_len);
    }
}

static bool send_media_ack(aap_session_t *s, uint8_t channel_id, int32_t session_id, uint32_t ack_tokens) {
    aap_protobuf_service_media_source_message_Ack ack = aap_protobuf_service_media_source_message_Ack_init_default;
    ack.session_id = session_id;
    ack.ack = ack_tokens;

    uint8_t pb_buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&stream, aap_protobuf_service_media_source_message_Ack_fields, &ack)) {
        return false;
    }

    uint8_t raw_msg[256];
    raw_msg[0] = (uint8_t)(AAP_MEDIA_MSG_ACK_IND >> 8);
    raw_msg[1] = (uint8_t)(AAP_MEDIA_MSG_ACK_IND & 0xFF);
    memcpy(raw_msg + 2, pb_buf, stream.bytes_written);
    size_t raw_len = stream.bytes_written + 2;

    if (aap_cryptor_is_active(s->cryptor)) {
        uint8_t cipher[512];
        size_t cipher_len = aap_cryptor_encrypt(s->cryptor, raw_msg, raw_len, cipher, sizeof(cipher));
        if (cipher_len == 0) return false;
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, false, true, cipher, cipher_len);
    } else {
        return send_raw_frame(s, channel_id, AAP_FRAME_BULK, false, false, raw_msg, raw_len);
    }
}

static void handle_control_message(aap_session_t *s, uint16_t msg_id, const uint8_t *payload, size_t payload_len) {
    switch (msg_id) {
        case AAP_MSG_VERSION_REQUEST: {
            printf("aap_session: received VersionRequest (raw)\n");
            uint8_t ver_resp[6];
            uint16_t major = htons(1);
            uint16_t minor = htons(6);
            uint16_t status = htons(0); /* STATUS_SUCCESS */
            memcpy(&ver_resp[0], &major, 2);
            memcpy(&ver_resp[2], &minor, 2);
            memcpy(&ver_resp[4], &status, 2);

            send_control_msg(s, AAP_MSG_VERSION_RESPONSE, ver_resp, sizeof(ver_resp), false);
            set_state(s, AAP_SESSION_STATE_TLS_HANDSHAKE, "Version handshake complete, starting TLS");

            /* Kick off TLS Client Hello */
            aap_cryptor_do_handshake(s->cryptor);
            uint8_t hs_buf[2048];
            size_t hs_len = aap_cryptor_read_handshake(s->cryptor, hs_buf, sizeof(hs_buf));
            if (hs_len > 0) {
                send_control_msg(s, AAP_MSG_SSL_HANDSHAKE, hs_buf, hs_len, false);
            }
            break;
        }

        case AAP_MSG_SSL_HANDSHAKE: {
            printf("aap_session: received SSLHandshake (%zu bytes)\n", payload_len);
            aap_cryptor_write_handshake(s->cryptor, payload, payload_len);
            bool done = aap_cryptor_do_handshake(s->cryptor);

            uint8_t hs_buf[2048];
            size_t hs_len = aap_cryptor_read_handshake(s->cryptor, hs_buf, sizeof(hs_buf));
            if (hs_len > 0) {
                send_control_msg(s, AAP_MSG_SSL_HANDSHAKE, hs_buf, hs_len, false);
            }

            if (done && aap_cryptor_is_active(s->cryptor)) {
                printf("aap_session: TLS handshake completed successfully! Sending AuthComplete\n");
                set_state(s, AAP_SESSION_STATE_AUTH, "TLS complete, authenticating");

                aap_protobuf_service_control_message_AuthResponse auth =
                    aap_protobuf_service_control_message_AuthResponse_init_default;
                auth.status = 0; /* STATUS_SUCCESS */

                uint8_t pb_buf[128];
                pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
                pb_encode(&stream, aap_protobuf_service_control_message_AuthResponse_fields, &auth);

                send_control_msg(s, AAP_MSG_AUTH_COMPLETE, pb_buf, stream.bytes_written, true);
            }
            break;
        }

        case AAP_MSG_SERVICE_DISCOVERY_REQUEST: {
            printf("aap_session: received ServiceDiscoveryRequest\n");
            aap_protobuf_service_control_message_ServiceDiscoveryResponse resp =
                aap_protobuf_service_control_message_ServiceDiscoveryResponse_init_default;

            resp.has_make = true;
            strncpy(resp.make, "Toyota", sizeof(resp.make) - 1);
            resp.has_model = true;
            strncpy(resp.model, "Prado", sizeof(resp.model) - 1);
            resp.has_year = true;
            strncpy(resp.year, "2018", sizeof(resp.year) - 1);

            resp.channels_count = 0;

            /* Channel 6: VideoSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_VIDEO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.video_configs_count = 1;
                ch->media_sink_service.video_configs[0].codec_resolution = aap_protobuf_service_media_sink_message_VideoCodecResolutionType_VIDEO_800x480;
                ch->media_sink_service.video_configs[0].frame_rate = aap_protobuf_service_media_sink_message_VideoFrameRateType_VIDEO_FPS_30;
                ch->media_sink_service.video_configs[0].density = 140;
                ch->media_sink_service.video_configs[0].real_density = 140;
            }

            /* Channel 1: MediaAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_MEDIA_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_MEDIA;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 48000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 2;
            }

            /* Channel 2: GuidanceAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_SPEECH_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_GUIDANCE;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 16000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 1;
            }

            /* Channel 3: SystemAudioSink */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_SYSTEM_AUDIO;
                ch->has_media_sink_service = true;
                ch->media_sink_service.available_type = aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
                ch->media_sink_service.audio_type = aap_protobuf_service_media_sink_message_AudioStreamType_AUDIO_STREAM_SYSTEM_AUDIO;
                ch->media_sink_service.available_while_in_call = true;
                ch->media_sink_service.audio_configs_count = 1;
                ch->media_sink_service.audio_configs[0].sampling_rate = 16000;
                ch->media_sink_service.audio_configs[0].number_of_bits = 16;
                ch->media_sink_service.audio_configs[0].number_of_channels = 1;
            }

            /* Channel 5: InputSource */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_INPUT;
                ch->has_input_source_service = true;
                ch->input_source_service.touchscreen_count = 1;
                ch->input_source_service.touchscreen[0].width = 800;
                ch->input_source_service.touchscreen[0].height = 480;
                ch->input_source_service.keycodes_supported_count = 3;
                ch->input_source_service.keycodes_supported[0] = 280;
                ch->input_source_service.keycodes_supported[1] = 281;
                ch->input_source_service.keycodes_supported[2] = 23;
            }

            /* Channel 4: SensorSource */
            {
                aap_protobuf_service_Service *ch = &resp.channels[resp.channels_count++];
                ch->id = AAP_CHANNEL_SENSOR;
                ch->has_sensor_source_service = true;
                ch->sensor_source_service.sensors_count = 2;
                ch->sensor_source_service.sensors[0].sensor_type = aap_protobuf_service_sensorsource_message_SensorType_SENSOR_DRIVING_STATUS_DATA;
                ch->sensor_source_service.sensors[1].sensor_type = aap_protobuf_service_sensorsource_message_SensorType_SENSOR_NIGHT_MODE;
            }

            uint8_t pb_buf[4096];
            pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&stream, aap_protobuf_service_control_message_ServiceDiscoveryResponse_fields, &resp);

            send_control_msg(s, AAP_MSG_SERVICE_DISCOVERY_RESPONSE, pb_buf, stream.bytes_written, true);
            set_state(s, AAP_SESSION_STATE_CHANNELS_OPENING, "Service discovery complete, opening channels");
            break;
        }

        case AAP_MSG_CHANNEL_OPEN_REQUEST: {
            aap_protobuf_service_control_message_ChannelOpenRequest req =
                aap_protobuf_service_control_message_ChannelOpenRequest_init_default;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
            pb_decode(&stream, aap_protobuf_service_control_message_ChannelOpenRequest_fields, &req);

            printf("aap_session: received ChannelOpenRequest (priority=%d)\n", req.priority);

            aap_protobuf_service_control_message_ChannelOpenResponse resp =
                aap_protobuf_service_control_message_ChannelOpenResponse_init_default;
            resp.status = aap_protobuf_shared_MessageStatus_STATUS_SUCCESS;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_ChannelOpenResponse_fields, &resp);

            send_control_msg(s, AAP_MSG_CHANNEL_OPEN_RESPONSE, pb_buf, ostream.bytes_written, true);
            set_state(s, AAP_SESSION_STATE_RUNNING, "Android Auto session running (Micro-AAP)");
            break;
        }

        case AAP_MSG_PING_REQUEST: {
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

            send_control_msg(s, AAP_MSG_PING_RESPONSE, pb_buf, ostream.bytes_written, true);
            break;
        }

        case AAP_MSG_BYE_BYE_REQUEST: {
            printf("aap_session: received ByeByeRequest\n");
            aap_protobuf_service_control_message_ByeByeResponse resp =
                aap_protobuf_service_control_message_ByeByeResponse_init_default;

            uint8_t pb_buf[64];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_control_message_ByeByeResponse_fields, &resp);

            send_control_msg(s, AAP_MSG_BYE_BYE_RESPONSE, pb_buf, ostream.bytes_written, true);
            set_state(s, AAP_SESSION_STATE_DISCONNECTED, "Phone requested disconnect (ByeBye)");
            break;
        }

        default:
            printf("aap_session: unhandled control message 0x%04X\n", msg_id);
            break;
    }
}

static void handle_media_channel(aap_session_t *s, uint8_t channel_id, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 2) return;
    uint16_t sub_cmd = (uint16_t)((payload[0] << 8) | payload[1]);
    const uint8_t *pb_data = payload + 2;
    size_t pb_len = payload_len - 2;

    switch (sub_cmd) {
        case AAP_MEDIA_MSG_SETUP_REQ: {
            printf("aap_session: channel %u setup request\n", channel_id);
            aap_protobuf_service_media_shared_message_Config cfg =
                aap_protobuf_service_media_shared_message_Config_init_default;
            cfg.status = aap_protobuf_service_media_shared_message_Config_Status_STATUS_READY;
            cfg.max_unacked = 8;
            cfg.configuration_indices_count = 1;
            cfg.configuration_indices[0] = 0;

            uint8_t pb_buf[128];
            pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
            pb_encode(&ostream, aap_protobuf_service_media_shared_message_Config_fields, &cfg);

            uint8_t raw_msg[256];
            raw_msg[0] = (uint8_t)(AAP_MEDIA_MSG_SETUP_RESP >> 8);
            raw_msg[1] = (uint8_t)(AAP_MEDIA_MSG_SETUP_RESP & 0xFF);
            memcpy(raw_msg + 2, pb_buf, ostream.bytes_written);

            uint8_t cipher[512];
            size_t cipher_len = aap_cryptor_encrypt(s->cryptor, raw_msg, ostream.bytes_written + 2, cipher, sizeof(cipher));
            if (cipher_len > 0) {
                send_raw_frame(s, channel_id, AAP_FRAME_BULK, false, true, cipher, cipher_len);
            }
            break;
        }

        case AAP_MEDIA_MSG_START_IND: {
            aap_protobuf_service_media_shared_message_Start start =
                aap_protobuf_service_media_shared_message_Start_init_default;
            pb_istream_t istream = pb_istream_from_buffer(pb_data, pb_len);
            pb_decode(&istream, aap_protobuf_service_media_shared_message_Start_fields, &start);

            printf("aap_session: channel %u start (session_id=%d)\n", channel_id, start.session_id);
            if (channel_id == AAP_CHANNEL_MEDIA_AUDIO) {
                s->media_session_id = start.session_id;
                aap_audio_sink_open(s->audio_media);
                aap_audio_sink_prepare(s->audio_media);
            } else if (channel_id == AAP_CHANNEL_VIDEO) {
                s->video_session_id = start.session_id;
                aap_video_sink_open(s->video_sink);
            }
            break;
        }

        case AAP_MEDIA_MSG_STOP_IND: {
            printf("aap_session: channel %u stop\n", channel_id);
            if (channel_id == AAP_CHANNEL_MEDIA_AUDIO) {
                aap_audio_sink_flush(s->audio_media);
            }
            break;
        }

        default: {
            /* Raw audio or video payload with 8-byte timestamp prefix */
            const uint8_t *stream_bytes = payload;
            size_t stream_bytes_len = payload_len;
            if (payload_len > 8) {
                stream_bytes = payload + 8;
                stream_bytes_len = payload_len - 8;
            }

            if (channel_id == AAP_CHANNEL_MEDIA_AUDIO) {
                aap_audio_sink_write(s->audio_media, stream_bytes, stream_bytes_len);
                send_media_ack(s, channel_id, s->media_session_id, 1);
            } else if (channel_id == AAP_CHANNEL_VIDEO) {
                aap_video_sink_decode(s->video_sink, stream_bytes, stream_bytes_len);
                send_media_ack(s, channel_id, s->video_session_id, 1);
            }
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

    s->last_ping_time = time(NULL);
    s->last_rx_time = time(NULL);

    set_state(s, AAP_SESSION_STATE_VERSION_HANDSHAKE, "Waiting for VersionRequest");
    return s;
}

void aap_session_destroy(aap_session_t *s) {
    if (!s) return;
    if (s->socket_fd >= 0) {
        close(s->socket_fd);
        s->socket_fd = -1;
    }
    aap_cryptor_destroy(s->cryptor);
    aap_audio_sink_destroy(s->audio_media);
    aap_audio_sink_destroy(s->audio_guidance);
    aap_audio_sink_destroy(s->audio_system);
    aap_video_sink_destroy(s->video_sink);
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

bool aap_session_process_incoming(aap_session_t *s) {
    if (!s || s->socket_fd < 0) return false;

    ssize_t n = read(s->socket_fd, s->rx_buf + s->rx_len, RX_BUFFER_SIZE - s->rx_len);
    if (n <= 0) {
        set_state(s, AAP_SESSION_STATE_DISCONNECTED, "Socket closed by peer");
        return false;
    }

    s->rx_len += (size_t)n;
    s->last_rx_time = time(NULL);

    while (s->rx_len >= 2) {
        aap_frame_header_t hdr;
        size_t hdr_len = aap_parse_frame_header(s->rx_buf, s->rx_len, &hdr);
        if (hdr_len == 0) {
            break; /* Incomplete header */
        }

        size_t frame_total = hdr_len + hdr.frame_size;
        if (s->rx_len < frame_total) {
            break; /* Incomplete payload */
        }

        const uint8_t *frame_payload = s->rx_buf + hdr_len;
        size_t payload_len = hdr.frame_size;

        uint8_t plain_buf[4096];
        const uint8_t *active_payload = frame_payload;
        size_t active_len = payload_len;

        if (hdr.flags & AAP_FLAG_ENCRYPTED) {
            size_t dec_len = aap_cryptor_decrypt(s->cryptor, frame_payload, payload_len, plain_buf, sizeof(plain_buf));
            if (dec_len > 0) {
                active_payload = plain_buf;
                active_len = dec_len;
            } else {
                fprintf(stderr, "aap_session: decrypt failed on channel %u\n", hdr.channel_id);
            }
        }

        if (hdr.channel_id == AAP_CHANNEL_CONTROL) {
            if (active_len >= 2) {
                uint16_t msg_id = (uint16_t)((active_payload[0] << 8) | active_payload[1]);
                handle_control_message(s, msg_id, active_payload + 2, active_len - 2);
            }
        } else {
            handle_media_channel(s, hdr.channel_id, active_payload, active_len);
        }

        /* Shift remaining buffer */
        memmove(s->rx_buf, s->rx_buf + frame_total, s->rx_len - frame_total);
        s->rx_len -= frame_total;
    }

    return true;
}

void aap_session_tick(aap_session_t *s) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    time_t now = time(NULL);
    if (now - s->last_ping_time >= 3) {
        s->last_ping_time = now;

        aap_protobuf_service_control_message_PingRequest ping =
            aap_protobuf_service_control_message_PingRequest_init_default;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ping.timestamp = (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;

        uint8_t pb_buf[128];
        pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
        pb_encode(&ostream, aap_protobuf_service_control_message_PingRequest_fields, &ping);

        send_control_msg(s, AAP_MSG_PING_REQUEST, pb_buf, ostream.bytes_written, true);
    }
}

void aap_session_send_key(aap_session_t *s, uint32_t keycode) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    aap_protobuf_service_inputsource_message_InputReport report =
        aap_protobuf_service_inputsource_message_InputReport_init_default;
    report.has_key_event = true;
    report.key_event.keys_count = 1;
    report.key_event.keys[0].keycode = keycode;
    report.key_event.keys[0].down = true;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_inputsource_message_InputReport_fields, &report);

    uint8_t cipher[512];
    size_t cipher_len = aap_cryptor_encrypt(s->cryptor, pb_buf, ostream.bytes_written, cipher, sizeof(cipher));
    if (cipher_len > 0) {
        send_raw_frame(s, AAP_CHANNEL_INPUT, AAP_FRAME_BULK, false, true, cipher, cipher_len);
    }
}

void aap_session_send_touch(aap_session_t *s, uint32_t x, uint32_t y, uint32_t action) {
    if (!s || s->state != AAP_SESSION_STATE_RUNNING) return;

    aap_protobuf_service_inputsource_message_InputReport report =
        aap_protobuf_service_inputsource_message_InputReport_init_default;
    report.has_touch_event = true;
    report.touch_event.has_action = true;
    report.touch_event.action = (aap_protobuf_service_inputsource_message_PointerAction)action;
    report.touch_event.pointer_data_count = 1;
    report.touch_event.pointer_data[0].x = x;
    report.touch_event.pointer_data[0].y = y;

    uint8_t pb_buf[256];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    pb_encode(&ostream, aap_protobuf_service_inputsource_message_InputReport_fields, &report);

    uint8_t cipher[512];
    size_t cipher_len = aap_cryptor_encrypt(s->cryptor, pb_buf, ostream.bytes_written, cipher, sizeof(cipher));
    if (cipher_len > 0) {
        send_raw_frame(s, AAP_CHANNEL_INPUT, AAP_FRAME_BULK, false, true, cipher, cipher_len);
    }
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

    uint8_t cipher[512];
    size_t cipher_len = aap_cryptor_encrypt(s->cryptor, pb_buf, ostream.bytes_written, cipher, sizeof(cipher));
    if (cipher_len > 0) {
        send_raw_frame(s, AAP_CHANNEL_SENSOR, AAP_FRAME_BULK, false, true, cipher, cipher_len);
    }
}

void aap_session_set_video_visible(aap_session_t *s, bool visible) {
    if (!s || !s->video_sink) return;
    aap_video_sink_set_visible(s->video_sink, visible);
}
