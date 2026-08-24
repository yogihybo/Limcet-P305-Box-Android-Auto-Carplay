#ifndef AAP_WIRE_H
#define AAP_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AAP Channel IDs */
typedef enum {
    AAP_CHANNEL_CONTROL       = 0,
    AAP_CHANNEL_MEDIA_AUDIO   = 1,
    AAP_CHANNEL_SPEECH_AUDIO  = 2,
    AAP_CHANNEL_SYSTEM_AUDIO  = 3,
    AAP_CHANNEL_SENSOR        = 4,
    AAP_CHANNEL_INPUT         = 5,
    AAP_CHANNEL_VIDEO         = 6,
    AAP_CHANNEL_BLUETOOTH     = 7,
    AAP_CHANNEL_NONE          = 255
} aap_channel_id_t;

/* AAP Frame Types (bits 0-1 of flags) */
typedef enum {
    AAP_FRAME_MIDDLE = 0x00,
    AAP_FRAME_FIRST  = 0x01,
    AAP_FRAME_LAST   = 0x02,
    AAP_FRAME_BULK   = 0x03  /* FIRST | LAST */
} aap_frame_type_t;

/* AAP Message & Encryption Flags */
#define AAP_FLAG_FRAME_TYPE_MASK 0x03
#define AAP_FLAG_CONTROL         0x04  /* Message type: 1 = control, 0 = specific */
#define AAP_FLAG_ENCRYPTED       0x08  /* 1 = encrypted, 0 = plain */

/* Header Sizes */
#define AAP_HEADER_SHORT_SIZE    4     /* 1 byte ch + 1 byte flags + 2 bytes frame_len */
#define AAP_HEADER_EXTENDED_SIZE 8     /* 1 byte ch + 1 byte flags + 2 bytes frame_len + 4 bytes total_len */

#define AAP_MAX_PAYLOAD_SIZE     16384

typedef struct {
    uint8_t  channel_id;
    uint8_t  flags;
    uint16_t frame_size;
    uint32_t total_size;     /* Valid only if FIRST or BULK */
} aap_frame_header_t;

/* Parse a frame header from raw buffer. Returns header length (4 or 8) on success, or 0 if buffer too short. */
size_t aap_parse_frame_header(const uint8_t *buf, size_t buf_len, aap_frame_header_t *out_hdr);

/* Pack a frame header into buffer. Returns header length (4 or 8) or 0 on error. */
size_t aap_pack_frame_header(uint8_t channel_id, aap_frame_type_t frame_type,
                             bool is_control, bool is_encrypted,
                             uint16_t frame_size, uint32_t total_size,
                             uint8_t *out_buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* AAP_WIRE_H */
