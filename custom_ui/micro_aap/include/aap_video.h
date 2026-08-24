#ifndef AAP_VIDEO_H
#define AAP_VIDEO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aap_video_sink aap_video_sink_t;

aap_video_sink_t *aap_video_sink_create(uint32_t width, uint32_t height);
void aap_video_sink_destroy(aap_video_sink_t *sink);

bool aap_video_sink_open(aap_video_sink_t *sink);
void aap_video_sink_close(aap_video_sink_t *sink);

/* Decode incoming H.264 NALU / stream chunk. Returns true if frame decoded & presented */
bool aap_video_sink_decode(aap_video_sink_t *sink, const uint8_t *nalu_data, size_t nalu_len);

void aap_video_sink_set_visible(aap_video_sink_t *sink, bool visible);

#ifdef __cplusplus
}
#endif

#endif /* AAP_VIDEO_H */
