#include "aap_wire.h"
#include <string.h>
#include <arpa/inet.h>

size_t aap_parse_frame_header(const uint8_t *buf, size_t buf_len, aap_frame_header_t *out_hdr) {
    if (!buf || !out_hdr || buf_len < 2) {
        return 0;
    }

    out_hdr->channel_id = buf[0];
    out_hdr->flags = buf[1];

    uint8_t frame_type = out_hdr->flags & AAP_FLAG_FRAME_TYPE_MASK;
    if (frame_type == AAP_FRAME_FIRST || frame_type == AAP_FRAME_BULK) {
        if (buf_len < AAP_HEADER_EXTENDED_SIZE) {
            return 0;
        }
        uint16_t fs_be;
        uint32_t ts_be;
        memcpy(&fs_be, &buf[2], sizeof(uint16_t));
        memcpy(&ts_be, &buf[4], sizeof(uint32_t));
        out_hdr->frame_size = ntohs(fs_be);
        out_hdr->total_size = ntohl(ts_be);
        return AAP_HEADER_EXTENDED_SIZE;
    } else {
        if (buf_len < AAP_HEADER_SHORT_SIZE) {
            return 0;
        }
        uint16_t fs_be;
        memcpy(&fs_be, &buf[2], sizeof(uint16_t));
        out_hdr->frame_size = ntohs(fs_be);
        out_hdr->total_size = out_hdr->frame_size;
        return AAP_HEADER_SHORT_SIZE;
    }
}

size_t aap_pack_frame_header(uint8_t channel_id, aap_frame_type_t frame_type,
                             bool is_control, bool is_encrypted,
                             uint16_t frame_size, uint32_t total_size,
                             uint8_t *out_buf, size_t max_len) {
    if (!out_buf) {
        return 0;
    }

    uint8_t flags = (uint8_t)frame_type;
    if (is_control) {
        flags |= AAP_FLAG_CONTROL;
    }
    if (is_encrypted) {
        flags |= AAP_FLAG_ENCRYPTED;
    }

    if (frame_type == AAP_FRAME_FIRST || frame_type == AAP_FRAME_BULK) {
        if (max_len < AAP_HEADER_EXTENDED_SIZE) {
            return 0;
        }
        out_buf[0] = channel_id;
        out_buf[1] = flags;
        uint16_t fs_be = htons(frame_size);
        uint32_t ts_be = htonl(total_size);
        memcpy(&out_buf[2], &fs_be, sizeof(uint16_t));
        memcpy(&out_buf[4], &ts_be, sizeof(uint32_t));
        return AAP_HEADER_EXTENDED_SIZE;
    } else {
        if (max_len < AAP_HEADER_SHORT_SIZE) {
            return 0;
        }
        out_buf[0] = channel_id;
        out_buf[1] = flags;
        uint16_t fs_be = htons(frame_size);
        memcpy(&out_buf[2], &fs_be, sizeof(uint16_t));
        return AAP_HEADER_SHORT_SIZE;
    }
}
