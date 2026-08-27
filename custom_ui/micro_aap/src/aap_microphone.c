#include "aap_microphone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <alsa/asoundlib.h>

#define MIC_SAMPLE_RATE 16000
#define MIC_CHANNELS    1
#define MIC_FRAMES_PER_CHUNK 160  /* 10ms = 160 frames @ 16kHz */
#define MIC_CHUNK_BYTES (MIC_FRAMES_PER_CHUNK * 2 * MIC_CHANNELS) /* 320 bytes */

struct aap_microphone {
    char device_name[64];
    snd_pcm_t *pcm_handle;
    pthread_t thread;
    bool thread_running;
    bool capturing;
    bool stop_flag;
    aap_session_t *session;
    aap_microphone_data_cb_t callback;
    unsigned int actual_channels;
    unsigned int actual_rate;
};

static uint64_t get_now_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static bool alsa_mic_open(aap_microphone_t *mic) {
    if (mic->pcm_handle) return true;

    const char *devs[] = {"plughw:0,1", "plughw:0,0", "hw:0,1", "default", "dsnoop"};
    int num_devs = sizeof(devs) / sizeof(devs[0]);

    for (int i = 0; i < num_devs; i++) {
        const char *dev = devs[i];
        if (!dev || strlen(dev) == 0) continue;

        int err = snd_pcm_open(&mic->pcm_handle, dev, SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) {
            fprintf(stderr, "[AA:MIC] snd_pcm_open('%s') failed: %s (%d)\n", dev, snd_strerror(err), err);
            continue;
        }

        snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(mic->pcm_handle, params);
        snd_pcm_hw_params_set_access(mic->pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(mic->pcm_handle, params, SND_PCM_FORMAT_S16_LE);

        unsigned int channels = MIC_CHANNELS;
        err = snd_pcm_hw_params_set_channels_near(mic->pcm_handle, params, &channels);
        if (err < 0) {
            fprintf(stderr, "[AA:MIC] '%s' set_channels_near failed: %s (%d)\n", dev, snd_strerror(err), err);
            snd_pcm_close(mic->pcm_handle);
            mic->pcm_handle = NULL;
            continue;
        }
        mic->actual_channels = channels;

        unsigned int rate = MIC_SAMPLE_RATE;
        snd_pcm_hw_params_set_rate_near(mic->pcm_handle, params, &rate, 0);
        mic->actual_rate = rate;

        snd_pcm_uframes_t period_size = MIC_FRAMES_PER_CHUNK;
        snd_pcm_hw_params_set_period_size_near(mic->pcm_handle, params, &period_size, 0);

        snd_pcm_uframes_t buffer_size = period_size * 8;
        snd_pcm_hw_params_set_buffer_size_near(mic->pcm_handle, params, &buffer_size);

        err = snd_pcm_hw_params(mic->pcm_handle, params);
        if (err < 0) {
            fprintf(stderr, "[AA:MIC] '%s' snd_pcm_hw_params failed: %s (%d)\n", dev, snd_strerror(err), err);
            snd_pcm_close(mic->pcm_handle);
            mic->pcm_handle = NULL;
            continue;
        }

        err = snd_pcm_prepare(mic->pcm_handle);
        if (err < 0) {
            fprintf(stderr, "[AA:MIC] '%s' snd_pcm_prepare failed: %s (%d)\n", dev, snd_strerror(err), err);
            snd_pcm_close(mic->pcm_handle);
            mic->pcm_handle = NULL;
            continue;
        }

        printf("[AA:MIC] successfully opened ALSA capture device '%s' (%u Hz, %u ch, period=%lu)\n",
               dev, rate, channels, (unsigned long)period_size);
        return true;
    }

    fprintf(stderr, "[AA:MIC] ERROR: failed to open any ALSA capture device\n");
    return false;
}

static void alsa_mic_close(aap_microphone_t *mic) {
    if (mic->pcm_handle) {
        snd_pcm_drop(mic->pcm_handle);
        snd_pcm_close(mic->pcm_handle);
        mic->pcm_handle = NULL;
    }
}

static void *mic_capture_thread(void *arg) {
    aap_microphone_t *mic = (aap_microphone_t *)arg;
    size_t raw_buffer_bytes = MIC_FRAMES_PER_CHUNK * 2 * mic->actual_channels;
    uint8_t *raw_buffer = (uint8_t *)malloc(raw_buffer_bytes);
    int16_t mono_buffer[MIC_FRAMES_PER_CHUNK];

    printf("[AA:MIC] capture worker started (actual_channels=%u)\n", mic->actual_channels);

    int chunk_cnt = 0;
    while (!mic->stop_flag && mic->capturing) {
        if (!mic->pcm_handle) {
            usleep(10000);
            continue;
        }

        snd_pcm_sframes_t frames = snd_pcm_readi(mic->pcm_handle, raw_buffer, MIC_FRAMES_PER_CHUNK);
        if (frames < 0) {
            int err = snd_pcm_recover(mic->pcm_handle, (int)frames, 0);
            if (err < 0) {
                fprintf(stderr, "[AA:MIC] ALSA read error on %s: %s\n", mic->device_name, snd_strerror(err));
                usleep(20000);
            }
            continue;
        }

        if (frames > 0 && mic->callback && mic->session) {
            uint64_t ts = get_now_micros();
            int16_t *src_samples = (int16_t *)raw_buffer;
            int16_t max_sample = 0;

            if (mic->actual_channels == 1) {
                for (size_t s_idx = 0; s_idx < (size_t)frames; ++s_idx) {
                    mono_buffer[s_idx] = src_samples[s_idx];
                    int16_t val = abs(src_samples[s_idx]);
                    if (val > max_sample) max_sample = val;
                }
            } else {
                /* Downmix stereo or multi-channel to mono */
                for (size_t s_idx = 0; s_idx < (size_t)frames; ++s_idx) {
                    int32_t sum = 0;
                    for (unsigned int c = 0; c < mic->actual_channels; ++c) {
                        sum += src_samples[s_idx * mic->actual_channels + c];
                    }
                    int16_t val = (int16_t)(sum / (int)mic->actual_channels);
                    mono_buffer[s_idx] = val;
                    int16_t abs_v = abs(val);
                    if (abs_v > max_sample) max_sample = abs_v;
                }
            }

            if (++chunk_cnt % 100 == 0) { // Log amplitude once per second
                printf("[AA:MIC] capture active: 100 chunks sent, peak_amplitude=%d/32767\n", max_sample);
            }

            size_t mono_bytes = (size_t)frames * sizeof(int16_t);
            mic->callback(mic->session, ts, (const uint8_t *)mono_buffer, mono_bytes);
        }
    }

    free(raw_buffer);
    alsa_mic_close(mic);
    printf("[AA:MIC] capture worker stopped\n");
    return NULL;
}



aap_microphone_t *aap_microphone_create(const char *device_name) {
    aap_microphone_t *mic = (aap_microphone_t *)calloc(1, sizeof(aap_microphone_t));
    if (!mic) return NULL;

    if (device_name && strlen(device_name) > 0) {
        strncpy(mic->device_name, device_name, sizeof(mic->device_name) - 1);
    } else {
        strncpy(mic->device_name, "default", sizeof(mic->device_name) - 1);
    }
    return mic;
}

void aap_microphone_destroy(aap_microphone_t *mic) {
    if (!mic) return;
    aap_microphone_stop(mic);
    free(mic);
}

bool aap_microphone_start(aap_microphone_t *mic, aap_session_t *session, aap_microphone_data_cb_t callback) {
    if (!mic) return false;
    if (mic->capturing) return true;

    if (!alsa_mic_open(mic)) {
        return false;
    }

    mic->session = session;
    mic->callback = callback;
    mic->capturing = true;
    mic->stop_flag = false;

    if (pthread_create(&mic->thread, NULL, mic_capture_thread, mic) != 0) {
        perror("pthread_create(mic_capture_thread)");
        mic->capturing = false;
        alsa_mic_close(mic);
        return false;
    }
    mic->thread_running = true;
    return true;
}

void aap_microphone_stop(aap_microphone_t *mic) {
    if (!mic) return;
    if (!mic->capturing && !mic->thread_running) return;

    mic->capturing = false;
    mic->stop_flag = true;

    if (mic->thread_running) {
        pthread_join(mic->thread, NULL);
        mic->thread_running = false;
    }
    alsa_mic_close(mic);
}

bool aap_microphone_is_capturing(const aap_microphone_t *mic) {
    return mic ? mic->capturing : false;
}
