#include "aap_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <alsa/asoundlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AAP_AUDIO_SLOTS 64
#define AAP_AUDIO_SLOT_MAX_SIZE 4096

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1[2], x2[2];
    float y1[2], y2[2];
    bool  active;
} biquad_filter_t;

typedef struct {
    uint8_t data[AAP_AUDIO_SLOT_MAX_SIZE];
    size_t  len;
} audio_slot_t;

struct aap_audio_sink {
    char device_name[64];
    uint32_t sample_rate;
    uint32_t channels;

    snd_pcm_t *pcm_handle;
    pthread_t thread;
    bool thread_running;
    bool stop_flag;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    /* 2026-09-05: real hardware bug found via code review -- snd_pcm_t
     * is not thread-safe for concurrent state-changing calls, but
     * snd_pcm_prepare() (called from the main session thread on
     * channel (re)start) and snd_pcm_writei()/snd_pcm_recover()
     * (called from audio_writer_thread below) both operated on
     * pcm_handle with no shared lock at all. This is deliberately a
     * SEPARATE mutex from `mutex` above (which guards the ring-buffer
     * producer/consumer state, e.g. aap_audio_sink_write() on the main
     * thread) rather than reusing it -- audio_writer_thread's actual
     * snd_pcm_writei() call can legitimately block for real time
     * (device busy, XRUN recovery's own backoff sleep), and holding
     * the ring-buffer mutex across that would stall
     * aap_audio_sink_write() on the main thread for just as long,
     * reintroducing the exact "blocks the main thread on real-time I/O"
     * problem this ring-buffer design exists to avoid. */
    pthread_mutex_t pcm_mutex;

    audio_slot_t slots[AAP_AUDIO_SLOTS];
    size_t read_idx;
    size_t write_idx;
    size_t count;

    /* Equalizer & Tone processing */
    pthread_mutex_t eq_mutex;
    biquad_filter_t eq_bass;
    biquad_filter_t eq_mid;
    biquad_filter_t eq_treble;
    biquad_filter_t eq_loudness_bass;
    biquad_filter_t eq_loudness_treble;
    bool            has_active_eq;
};

static void init_biquad_low_shelf(biquad_filter_t *f, float f0, float gain_db, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = false;
        memset(f->x1, 0, sizeof(f->x1));
        memset(f->x2, 0, sizeof(f->x2));
        memset(f->y1, 0, sizeof(f->y1));
        memset(f->y2, 0, sizeof(f->y2));
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / 2.0f * 1.41421356f; /* S=1 -> sqrt(2) */

    float a0 = (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha;
    f->b0 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha)) / a0;
    f->b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w)) / a0;
    f->b2 = (A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha)) / a0;
    f->a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w)) / a0;
    f->a2 = ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha) / a0;
    f->active = true;
}

static void init_biquad_peaking(biquad_filter_t *f, float f0, float gain_db, float Q, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = false;
        memset(f->x1, 0, sizeof(f->x1));
        memset(f->x2, 0, sizeof(f->x2));
        memset(f->y1, 0, sizeof(f->y1));
        memset(f->y2, 0, sizeof(f->y2));
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * Q);

    float a0 = 1.0f + alpha / A;
    f->b0 = (1.0f + alpha * A) / a0;
    f->b1 = (-2.0f * cos_w) / a0;
    f->b2 = (1.0f - alpha * A) / a0;
    f->a1 = (-2.0f * cos_w) / a0;
    f->a2 = (1.0f - alpha / A) / a0;
    f->active = true;
}

static void init_biquad_high_shelf(biquad_filter_t *f, float f0, float gain_db, float fs) {
    if (fabsf(gain_db) < 0.1f) {
        f->active = false;
        memset(f->x1, 0, sizeof(f->x1));
        memset(f->x2, 0, sizeof(f->x2));
        memset(f->y1, 0, sizeof(f->y1));
        memset(f->y2, 0, sizeof(f->y2));
        return;
    }
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / 2.0f * 1.41421356f;

    float a0 = (A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha;
    f->b0 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtf(A) * alpha)) / a0;
    f->b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w)) / a0;
    f->b2 = (A * ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha)) / a0;
    f->a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w)) / a0;
    f->a2 = ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtf(A) * alpha) / a0;
    f->active = true;
}

static inline int16_t clamp_s16(int32_t val) {
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

static inline float process_biquad(biquad_filter_t *f, float in, int ch) {
    if (!f->active) return in;
    float out = f->b0 * in + f->b1 * f->x1[ch] + f->b2 * f->x2[ch]
                - f->a1 * f->y1[ch] - f->a2 * f->y2[ch];
    f->x2[ch] = f->x1[ch];
    f->x1[ch] = in;
    f->y2[ch] = f->y1[ch];
    f->y1[ch] = out;
    return out;
}

static void *audio_writer_thread(void *arg) {
    aap_audio_sink_t *sink = (aap_audio_sink_t *)arg;

    while (1) {
        audio_slot_t slot;
        pthread_mutex_lock(&sink->mutex);
        while (sink->count == 0 && !sink->stop_flag) {
            pthread_cond_wait(&sink->cond, &sink->mutex);
        }

        if (sink->stop_flag && sink->count == 0) {
            pthread_mutex_unlock(&sink->mutex);
            break;
        }

        slot = sink->slots[sink->read_idx];
        sink->read_idx = (sink->read_idx + 1) % AAP_AUDIO_SLOTS;
        sink->count--;
        pthread_mutex_unlock(&sink->mutex);

        if (slot.len == 0 || !sink->pcm_handle) {
            continue;
        }

        uint32_t bytes_per_frame = 2 * sink->channels; /* 16-bit PCM */
        snd_pcm_uframes_t frames_left = slot.len / bytes_per_frame;
        const uint8_t *cursor = slot.data;

        int recovery_attempts = 0;
        /* 2026-09-05: see this file's own comment on pcm_mutex --
         * guards every snd_pcm_t state-changing call against
         * aap_audio_sink_prepare() (main thread), which is the ONLY
         * other place pcm_handle is touched outside this thread.
         * Locked for the whole per-slot write loop, not per-call, so a
         * prepare() can never land in the middle of writing one
         * contiguous slot's worth of frames. */
        pthread_mutex_lock(&sink->pcm_mutex);
        while (frames_left > 0 && !sink->stop_flag) {
            snd_pcm_sframes_t written = snd_pcm_writei(sink->pcm_handle, cursor, frames_left);
            if (written < 0) {
                if (++recovery_attempts > 10) {
                    fprintf(stderr, "[AA] giving up on %s after 10 errors\n", sink->device_name);
                    break;
                }
                int err = snd_pcm_recover(sink->pcm_handle, (int)written, 1);
                if (err < 0) {
                    fprintf(stderr, "[AA] unrecoverable write error on %s: %s\n",
                            sink->device_name, snd_strerror(err));
                    break;
                }
                usleep(5000); /* 5ms backoff */
                continue;
            }
            if (written == 0) {
                break;
            }

            cursor += (size_t)written * bytes_per_frame;
            frames_left -= (snd_pcm_uframes_t)written;
        }
        pthread_mutex_unlock(&sink->pcm_mutex);
    }

    return NULL;
}

aap_audio_sink_t *aap_audio_sink_create(const char *device_name, uint32_t sample_rate, uint32_t channels) {
    aap_audio_sink_t *sink = (aap_audio_sink_t *)calloc(1, sizeof(aap_audio_sink_t));
    if (!sink) return NULL;

    strncpy(sink->device_name, device_name ? device_name : "plug:softvol2", sizeof(sink->device_name) - 1);
    sink->sample_rate = sample_rate ? sample_rate : 48000;
    sink->channels = channels ? channels : 2;

    pthread_mutex_init(&sink->mutex, NULL);
    pthread_cond_init(&sink->cond, NULL);
    pthread_mutex_init(&sink->eq_mutex, NULL);
    pthread_mutex_init(&sink->pcm_mutex, NULL);

    return sink;
}

void aap_audio_sink_destroy(aap_audio_sink_t *sink) {
    if (!sink) return;
    aap_audio_sink_close(sink);
    pthread_mutex_destroy(&sink->mutex);
    pthread_cond_destroy(&sink->cond);
    pthread_mutex_destroy(&sink->eq_mutex);
    pthread_mutex_destroy(&sink->pcm_mutex);
    free(sink);
}

void aap_audio_sink_set_eq(aap_audio_sink_t *sink, int bass_db, int mid_db, int treble_db, bool loudness) {
    if (!sink) return;
    pthread_mutex_lock(&sink->eq_mutex);

    float fs = (float)sink->sample_rate;
    init_biquad_low_shelf(&sink->eq_bass, 100.0f, (float)bass_db, fs);
    init_biquad_peaking(&sink->eq_mid, 1000.0f, (float)mid_db, 1.0f, fs);
    init_biquad_high_shelf(&sink->eq_treble, 8000.0f, (float)treble_db, fs);

    if (loudness) {
        init_biquad_low_shelf(&sink->eq_loudness_bass, 80.0f, 3.0f, fs);
        init_biquad_high_shelf(&sink->eq_loudness_treble, 10000.0f, 2.5f, fs);
    } else {
        sink->eq_loudness_bass.active = false;
        sink->eq_loudness_treble.active = false;
    }

    sink->has_active_eq = (sink->eq_bass.active || sink->eq_mid.active || sink->eq_treble.active ||
                           sink->eq_loudness_bass.active || sink->eq_loudness_treble.active);

    pthread_mutex_unlock(&sink->eq_mutex);
}

bool aap_audio_sink_open(aap_audio_sink_t *sink) {
    if (!sink) return false;
    if (sink->pcm_handle) return true;

    int err = snd_pcm_open(&sink->pcm_handle, sink->device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[AA] snd_pcm_open(%s) failed: %s\n", sink->device_name, snd_strerror(err));
        sink->pcm_handle = NULL;
        return false;
    }

    err = snd_pcm_set_params(sink->pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             sink->channels, sink->sample_rate, 1, 200000); /* 200ms latency */
    if (err < 0) {
        fprintf(stderr, "[AA] snd_pcm_set_params(%s) failed: %s\n", sink->device_name, snd_strerror(err));
        snd_pcm_close(sink->pcm_handle);
        sink->pcm_handle = NULL;
        return false;
    }

    sink->stop_flag = false;
    sink->read_idx = 0;
    sink->write_idx = 0;
    sink->count = 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024); /* 128KB stack */
    sink->thread_running = (pthread_create(&sink->thread, &attr, audio_writer_thread, sink) == 0);
    pthread_attr_destroy(&attr);

    printf("[AA] opened %s (%u Hz, 16-bit, %u ch)\n", sink->device_name, sink->sample_rate, sink->channels);
    return true;
}

void aap_audio_sink_close(aap_audio_sink_t *sink) {
    if (!sink) return;

    if (sink->thread_running) {
        pthread_mutex_lock(&sink->mutex);
        sink->stop_flag = true;
        pthread_cond_broadcast(&sink->cond);
        pthread_mutex_unlock(&sink->mutex);

        pthread_join(sink->thread, NULL);
        sink->thread_running = false;
    }

    if (sink->pcm_handle) {
        snd_pcm_close(sink->pcm_handle);
        sink->pcm_handle = NULL;
    }
}

void aap_audio_sink_flush(aap_audio_sink_t *sink) {
    if (!sink) return;
    pthread_mutex_lock(&sink->mutex);
    sink->read_idx = 0;
    sink->write_idx = 0;
    sink->count = 0;
    pthread_mutex_unlock(&sink->mutex);
}

void aap_audio_sink_prepare(aap_audio_sink_t *sink) {
    if (!sink || !sink->pcm_handle) return;
    /* 2026-09-05: real hardware bug found via code review -- this
     * called snd_pcm_prepare() with zero locking, while
     * audio_writer_thread calls snd_pcm_writei()/snd_pcm_recover() on
     * the SAME snd_pcm_t. snd_pcm_t is not thread-safe for concurrent
     * state-changing calls from different threads -- on a fresh
     * channel open this was harmless (the writer thread has nothing
     * queued yet, parked in pthread_cond_wait when this runs), but a
     * channel can legitimately receive a SECOND MEDIA_MESSAGE_START
     * while already open and actively writing (aap_audio_sink_open()
     * itself already handles this -- a no-op if pcm_handle is set --
     * but this call was never guarded the same way), which could race
     * a real in-flight snd_pcm_writei()/snd_pcm_recover() call. Uses
     * pcm_mutex (see this file's own struct-field comment) rather than
     * the ring-buffer `mutex` -- that one is briefly held by
     * aap_audio_sink_write() on the main thread just to enqueue, and
     * must never be held across a real ALSA I/O call. */
    pthread_mutex_lock(&sink->pcm_mutex);
    snd_pcm_prepare(sink->pcm_handle);
    pthread_mutex_unlock(&sink->pcm_mutex);
}

bool aap_audio_sink_write(aap_audio_sink_t *sink, const uint8_t *pcm_data, size_t pcm_len) {
    if (!sink || !pcm_data || pcm_len == 0) return false;

    pthread_mutex_lock(&sink->mutex);
    size_t offset = 0;
    while (offset < pcm_len) {
        size_t chunk = pcm_len - offset;
        if (chunk > AAP_AUDIO_SLOT_MAX_SIZE) {
            chunk = AAP_AUDIO_SLOT_MAX_SIZE;
        }

        if (sink->count >= AAP_AUDIO_SLOTS) {
            /* Drop oldest buffer in static ring */
            sink->read_idx = (sink->read_idx + 1) % AAP_AUDIO_SLOTS;
            sink->count--;
        }

        audio_slot_t *slot = &sink->slots[sink->write_idx];
        memcpy(slot->data, pcm_data + offset, chunk);
        slot->len = chunk;

        if (sink->has_active_eq && sink->channels == 2) {
            int16_t *samples = (int16_t *)slot->data;
            size_t num_samples = chunk / sizeof(int16_t);
            pthread_mutex_lock(&sink->eq_mutex);
            for (size_t s = 0; s < num_samples; s += 2) {
                float l = (float)samples[s];
                float r = (float)samples[s + 1];

                l = process_biquad(&sink->eq_bass, l, 0);
                r = process_biquad(&sink->eq_bass, r, 1);

                l = process_biquad(&sink->eq_mid, l, 0);
                r = process_biquad(&sink->eq_mid, r, 1);

                l = process_biquad(&sink->eq_treble, l, 0);
                r = process_biquad(&sink->eq_treble, r, 1);

                l = process_biquad(&sink->eq_loudness_bass, l, 0);
                r = process_biquad(&sink->eq_loudness_bass, r, 1);

                l = process_biquad(&sink->eq_loudness_treble, l, 0);
                r = process_biquad(&sink->eq_loudness_treble, r, 1);

                samples[s] = clamp_s16((int32_t)l);
                samples[s + 1] = clamp_s16((int32_t)r);
            }
            pthread_mutex_unlock(&sink->eq_mutex);
        }

        sink->write_idx = (sink->write_idx + 1) % AAP_AUDIO_SLOTS;
        sink->count++;
        offset += chunk;
    }

    pthread_cond_signal(&sink->cond);
    pthread_mutex_unlock(&sink->mutex);
    return true;
}

size_t aap_audio_sink_queued_buffers(const aap_audio_sink_t *sink) {
    if (!sink) return 0;
    return sink->count;
}
