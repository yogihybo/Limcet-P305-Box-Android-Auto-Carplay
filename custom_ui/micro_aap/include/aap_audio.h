#ifndef AAP_AUDIO_H
#define AAP_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aap_audio_sink aap_audio_sink_t;

aap_audio_sink_t *aap_audio_sink_create(const char *device_name, uint32_t sample_rate, uint32_t channels);
void aap_audio_sink_destroy(aap_audio_sink_t *sink);

bool aap_audio_sink_open(aap_audio_sink_t *sink);
void aap_audio_sink_close(aap_audio_sink_t *sink);
void aap_audio_sink_flush(aap_audio_sink_t *sink);
void aap_audio_sink_prepare(aap_audio_sink_t *sink);

/* Write PCM frame bytes (interleaved 16-bit) into pre-allocated ring buffer */
bool aap_audio_sink_write(aap_audio_sink_t *sink, const uint8_t *pcm_data, size_t pcm_len);

/* Configure 3-Band Parametric Equalizer and dynamic loudness */
void aap_audio_sink_set_eq(aap_audio_sink_t *sink, int bass_db, int mid_db, int treble_db, bool loudness);

size_t aap_audio_sink_queued_buffers(const aap_audio_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif /* AAP_AUDIO_H */
