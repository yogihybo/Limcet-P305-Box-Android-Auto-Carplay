#include "aap_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#define AAP_AUDIO_SLOTS 64
#define AAP_AUDIO_SLOT_MAX_SIZE 4096

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

    audio_slot_t slots[AAP_AUDIO_SLOTS];
    size_t read_idx;
    size_t write_idx;
    size_t count;
};

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
        while (frames_left > 0 && !sink->stop_flag) {
            snd_pcm_sframes_t written = snd_pcm_writei(sink->pcm_handle, cursor, frames_left);
            if (written < 0) {
                if (++recovery_attempts > 10) {
                    fprintf(stderr, "aap_audio: giving up on %s after 10 errors\n", sink->device_name);
                    break;
                }
                int err = snd_pcm_recover(sink->pcm_handle, (int)written, 1);
                if (err < 0) {
                    fprintf(stderr, "aap_audio: unrecoverable write error on %s: %s\n",
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

    return sink;
}

void aap_audio_sink_destroy(aap_audio_sink_t *sink) {
    if (!sink) return;
    aap_audio_sink_close(sink);
    pthread_mutex_destroy(&sink->mutex);
    pthread_cond_destroy(&sink->cond);
    free(sink);
}

bool aap_audio_sink_open(aap_audio_sink_t *sink) {
    if (!sink) return false;
    if (sink->pcm_handle) return true;

    int err = snd_pcm_open(&sink->pcm_handle, sink->device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "aap_audio: snd_pcm_open(%s) failed: %s\n", sink->device_name, snd_strerror(err));
        sink->pcm_handle = NULL;
        return false;
    }

    err = snd_pcm_set_params(sink->pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             sink->channels, sink->sample_rate, 1, 200000); /* 200ms latency */
    if (err < 0) {
        fprintf(stderr, "aap_audio: snd_pcm_set_params(%s) failed: %s\n", sink->device_name, snd_strerror(err));
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

    printf("aap_audio: opened %s (%u Hz, 16-bit, %u ch)\n", sink->device_name, sink->sample_rate, sink->channels);
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
    snd_pcm_prepare(sink->pcm_handle);
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
