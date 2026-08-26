#include "androidauto/alsa_input.h"
#include "androidauto/log_timing.h"

#include <cstdio>

namespace androidauto {

AlsaInput::AlsaInput(std::string deviceName, uint32_t sampleRate, uint32_t bitsPerSample, uint32_t channels)
    : deviceName_(std::move(deviceName)), sampleRate_(sampleRate), bitsPerSample_(bitsPerSample), channels_(channels) {}

AlsaInput::~AlsaInput() {
    close();
}

bool AlsaInput::open() {
    if (bitsPerSample_ != 16) {
        std::fprintf(stderr, "%s androidauto::AlsaInput: only 16-bit PCM supported, got %u\n", androidauto::logTimestamp().c_str(),
                     bitsPerSample_);
        return false;
    }

    int err = snd_pcm_open(&pcmHandle_, deviceName_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        std::fprintf(stderr, "%s androidauto::AlsaInput: snd_pcm_open(%s) failed: %s\n", androidauto::logTimestamp().c_str(),
                     deviceName_.c_str(), snd_strerror(err));
        pcmHandle_ = nullptr;
        return false;
    }

    // 100ms latency -- generous enough to absorb single-core scheduling
    // jitter (same reasoning as AlsaOutput's 200ms, halved since this
    // is a much smaller mono/16kHz stream and a shorter buffer keeps
    // voice-input latency lower, which matters more for a live
    // conversation than for media playback).
    err = snd_pcm_set_params(pcmHandle_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, channels_, sampleRate_,
                              1, 100000);
    if (err < 0) {
        std::fprintf(stderr, "%s androidauto::AlsaInput: snd_pcm_set_params(%s) failed: %s\n", androidauto::logTimestamp().c_str(),
                     deviceName_.c_str(), snd_strerror(err));
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return false;
    }

    std::printf("%s androidauto::AlsaInput: opened %s (%u Hz, 16-bit, %u ch)\n", androidauto::logTimestamp().c_str(), deviceName_.c_str(),
                sampleRate_, channels_);
    return true;
}

int AlsaInput::read(void * outBuffer, uint32_t frameCount) {
    if (!pcmHandle_) return -1;

    snd_pcm_sframes_t got = snd_pcm_readi(pcmHandle_, outBuffer, frameCount);
    if (got < 0) {
        int recovered = snd_pcm_recover(pcmHandle_, static_cast<int>(got), 1);
        if (recovered < 0) {
            std::fprintf(stderr, "%s androidauto::AlsaInput: unrecoverable read error on %s: %s\n", androidauto::logTimestamp().c_str(),
                         deviceName_.c_str(), snd_strerror(recovered));
            return -1;
        }
        got = snd_pcm_readi(pcmHandle_, outBuffer, frameCount);
        if (got < 0) {
            std::fprintf(stderr, "%s androidauto::AlsaInput: read failed even after recovery on %s: %s\n",
                         androidauto::logTimestamp().c_str(), deviceName_.c_str(), snd_strerror(static_cast<int>(got)));
            return -1;
        }
    }
    return static_cast<int>(got);
}

void AlsaInput::close() {
    if (pcmHandle_) {
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }
}

}  // namespace androidauto
