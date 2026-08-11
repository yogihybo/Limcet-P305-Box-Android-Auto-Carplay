#include "androidauto/alsa_output.h"

#include <cstdio>

#include <dlfcn.h>

namespace androidauto {

namespace {
// Real, stable, public ALSA enum values (alsa/asoundlib.h) -- not
// reverse-engineered, these have been ABI-stable across every ALSA
// release relevant here.
constexpr int kSndPcmStreamPlayback = 0;
constexpr int kSndPcmFormatS16Le = 2;
constexpr int kSndPcmAccessRwInterleaved = 3;

// Real path confirmed present on the target rootfs (firmware_source/
// mtd6_rootfs/usr/lib/libasound.so.2.0.0) -- no libasound.so/
// libasound.so.2 symlink exists there, so the fully-versioned name is
// used directly, same as libmfc.so's own exact-path dlopen.
constexpr const char * kLibPath = "/usr/lib/libasound.so.2.0.0";
}  // namespace

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

    lib_ = dlopen(kLibPath, RTLD_NOW);
    if (!lib_) {
        std::fprintf(stderr, "androidauto::AlsaOutput: dlopen(%s) failed: %s\n", kLibPath,
                     dlerror());
        return false;
    }

    snd_pcm_open_ = reinterpret_cast<decltype(snd_pcm_open_)>(dlsym(lib_, "snd_pcm_open"));
    snd_pcm_set_params_ =
        reinterpret_cast<decltype(snd_pcm_set_params_)>(dlsym(lib_, "snd_pcm_set_params"));
    snd_pcm_writei_ = reinterpret_cast<decltype(snd_pcm_writei_)>(dlsym(lib_, "snd_pcm_writei"));
    snd_pcm_recover_ = reinterpret_cast<decltype(snd_pcm_recover_)>(dlsym(lib_, "snd_pcm_recover"));
    snd_pcm_close_ = reinterpret_cast<decltype(snd_pcm_close_)>(dlsym(lib_, "snd_pcm_close"));
    snd_strerror_ = reinterpret_cast<decltype(snd_strerror_)>(dlsym(lib_, "snd_strerror"));

    if (!snd_pcm_open_ || !snd_pcm_set_params_ || !snd_pcm_writei_ || !snd_pcm_recover_ ||
        !snd_pcm_close_ || !snd_strerror_) {
        std::fprintf(stderr, "androidauto::AlsaOutput: dlsym failed to resolve one or more "
                     "snd_pcm_* symbols: %s\n", dlerror());
        dlclose(lib_);
        lib_ = nullptr;
        return false;
    }

    int err = snd_pcm_open_(&pcmHandle_, deviceName_.c_str(), kSndPcmStreamPlayback, 0);
    if (err < 0) {
        std::fprintf(stderr, "androidauto::AlsaOutput: snd_pcm_open(%s) failed: %s\n",
                     deviceName_.c_str(), snd_strerror_(err));
        pcmHandle_ = nullptr;
        return false;
    }

    // 200ms latency -- generous enough to absorb single-core scheduling
    // jitter (matches this project's own real audio-stutter findings,
    // docs/AUDIO_SUBSYSTEM_INVESTIGATION.md: dmix's own buffer was
    // doubled for exactly this reason), small enough not to add
    // noticeable lag for a media stream. Not yet hardware-tuned.
    err = snd_pcm_set_params_(pcmHandle_, kSndPcmFormatS16Le, kSndPcmAccessRwInterleaved,
                              channels_, sampleRate_, 1, 200000);
    if (err < 0) {
        std::fprintf(stderr, "androidauto::AlsaOutput: snd_pcm_set_params(%s) failed: %s\n",
                     deviceName_.c_str(), snd_strerror_(err));
        snd_pcm_close_(pcmHandle_);
        pcmHandle_ = nullptr;
        return false;
    }

    std::printf("androidauto::AlsaOutput: opened %s (%u Hz, 16-bit, %u ch)\n", deviceName_.c_str(),
               sampleRate_, channels_);
    return true;
}

bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
    if (!pcmHandle_) return false;

    long written = snd_pcm_writei_(pcmHandle_, interleavedSamples, frameCount);
    if (written < 0) {
        // ALSA's own documented XRUN-recovery pattern: retry once via
        // snd_pcm_recover(), then re-attempt the write.
        int recovered = snd_pcm_recover_(pcmHandle_, static_cast<int>(written), 1);
        if (recovered < 0) {
            std::fprintf(stderr, "androidauto::AlsaOutput: unrecoverable write error on %s: %s\n",
                         deviceName_.c_str(), snd_strerror_(recovered));
            return false;
        }
        written = snd_pcm_writei_(pcmHandle_, interleavedSamples, frameCount);
        if (written < 0) {
            std::fprintf(stderr, "androidauto::AlsaOutput: write failed even after recovery on "
                         "%s: %s\n", deviceName_.c_str(), snd_strerror_(static_cast<int>(written)));
            return false;
        }
    }
    return true;
}

void AlsaOutput::close() {
    if (pcmHandle_ && snd_pcm_close_) {
        snd_pcm_close_(pcmHandle_);
        pcmHandle_ = nullptr;
    }
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
}

}  // namespace androidauto
