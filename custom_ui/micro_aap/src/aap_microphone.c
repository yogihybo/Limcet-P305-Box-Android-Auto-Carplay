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
};

static uint64_t get_now_micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static bool alsa_mic_open(aap_microphone_t *mic) {
    if (mic->pcm_handle) return true;

    const char *devs[] = {mic->device_name, "default", "dsnoop", "hw:0,1"};
    int num_devs = sizeof(devs) / sizeof(devs[0]);

    for (int i = 0; i < num_devs; i++) {
        const char *dev = devs[i];
        if (!dev || strlen(dev) == 0) continue;

        int err = snd_pcm_open(&mic->pcm_handle, dev, SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) {
            continue;
        }

        snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(mic->pcm_handle, params);
        snd_pcm_hw_params_set_access(mic->pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(mic->pcm_handle, params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(mic->pcm_handle, params, MIC_CHANNELS);

        unsigned int rate = MIC_SAMPLE_RATE;
        snd_pcm_hw_params_set_rate_near(mic->pcm_handle, params, &rate, 0);

        snd_pcm_uframes_t period_size = MIC_FRAMES_PER_CHUNK;
        snd_pcm_hw_params_set_period_size_near(mic->pcm_handle, params, &period_size, 0);

        snd_pcm_uframes_t buffer_size = period_size * 8;
        snd_pcm_hw_params_set_buffer_size_near(mic->pcm_handle, params, &buffer_size);

        err = snd_pcm_hw_params(mic->pcm_handle, params);
        if (err < 0) {
            snd_pcm_close(mic->pcm_handle);
            mic->pcm_handle = NULL;
            continue;
        }

        err = snd_pcm_prepare(mic->pcm_handle);
        if (err < 0) {
            snd_pcm_close(mic->pcm_handle);
            mic->pcm_handle = NULL;
            continue;
        }

        printf("aap_microphone: successfully opened ALSA capture device '%s' (%u Hz, %d ch)\n",
               dev, rate, MIC_CHANNELS);
        return true;
    }

    fprintf(stderr, "aap_microphone: failed to open any ALSA capture device\n");
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
    uint8_t buffer[MIC_CHUNK_BYTES];

    printf("aap_microphone: capture worker started\n");

    while (!mic->stop_flag && mic->capturing) {
        if (!mic->pcm_handle) {
            usleep(10000);
            continue;
        }

        snd_pcm_sframes_t frames = snd_pcm_readi(mic->pcm_handle, buffer, MIC_FRAMES_PER_CHUNK);
        if (frames < 0) {
            int err = snd_pcm_recover(mic->pcm_handle, (int)frames, 0);
            if (err < 0) {
                fprintf(stderr, "aap_microphone: ALSA read error on %s: %s\n", mic->device_name, snd_strerror(err));
                usleep(20000);
            }
            continue;
        }

        if (frames > 0 && mic->callback && mic->session) {
            uint64_t ts = get_now_micros();
            size_t bytes = (size_t)frames * 2 * MIC_CHANNELS;
            mic->callback(mic->session, ts, buffer, bytes);
        }
    }

    alsa_mic_close(mic);
    printf("aap_microphone: capture worker stopped\n");
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
