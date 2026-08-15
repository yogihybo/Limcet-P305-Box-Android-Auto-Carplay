#include "androidauto/alsa_output.h"

#include <cstdio>

namespace androidauto {

AlsaOutput::AlsaOutput(std::string deviceName, uint32_t sampleRate, uint32_t bitsPerSample,
                       uint32_t channels)
    : deviceName_(std::move(deviceName)),
      sampleRate_(sampleRate),
      bitsPerSample_(bitsPerSample),
      channels_(channels) {}

AlsaOutput::~AlsaOutput() {
    close();
}

bool AlsaOutput::open() {
    if (bitsPerSample_ != 16) {
        std::fprintf(stderr, "androidauto::AlsaOutput: only 16-bit PCM supported, got %u\n",
                     bitsPerSample_);
        return false;
    }

    int err = snd_pcm_open(&pcmHandle_, deviceName_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::fprintf(stderr, "androidauto::AlsaOutput: snd_pcm_open(%s) failed: %s\n",
                     deviceName_.c_str(), snd_strerror(err));
        pcmHandle_ = nullptr;
        return false;
    }

    // 200ms latency -- generous enough to absorb single-core scheduling
    // jitter (matches this project's own real audio-stutter findings,
    // docs/AUDIO_SUBSYSTEM_INVESTIGATION.md: dmix's own buffer was
    // doubled for exactly this reason), small enough not to add
    // noticeable lag for a media stream. Not yet hardware-tuned.
    err = snd_pcm_set_params(pcmHandle_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              channels_, sampleRate_, 1, 200000);
    if (err < 0) {
        std::fprintf(stderr, "androidauto::AlsaOutput: snd_pcm_set_params(%s) failed: %s\n",
                     deviceName_.c_str(), snd_strerror(err));
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return false;
    }

    std::printf("androidauto::AlsaOutput: opened %s (%u Hz, 16-bit, %u ch)\n", deviceName_.c_str(),
               sampleRate_, channels_);
    return true;
}

bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
    if (!pcmHandle_) return false;

    snd_pcm_sframes_t written = snd_pcm_writei(pcmHandle_, interleavedSamples, frameCount);
    if (written < 0) {
        // ALSA's own documented XRUN-recovery pattern: retry once via
        // snd_pcm_recover(), then re-attempt the write.
        int recovered = snd_pcm_recover(pcmHandle_, static_cast<int>(written), 1);
        if (recovered < 0) {
            std::fprintf(stderr, "androidauto::AlsaOutput: unrecoverable write error on %s: %s\n",
                         deviceName_.c_str(), snd_strerror(recovered));
            return false;
        }
        written = snd_pcm_writei(pcmHandle_, interleavedSamples, frameCount);
        if (written < 0) {
            std::fprintf(stderr, "androidauto::AlsaOutput: write failed even after recovery on "
                         "%s: %s\n", deviceName_.c_str(), snd_strerror(static_cast<int>(written)));
            return false;
        }
    }
    return true;
}

void AlsaOutput::close() {
    if (pcmHandle_) {
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }
}

}  // namespace androidauto
