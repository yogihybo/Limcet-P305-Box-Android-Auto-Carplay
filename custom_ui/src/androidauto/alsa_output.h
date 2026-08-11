// Minimal ALSA PCM playback wrapper, dlopen'd at runtime against the
// target rootfs's own libasound.so.2.0.0 -- NOT statically linked, and
// deliberately not built against alsa-lib's own headers (not
// cross-compiled/vendored anywhere in this project). Matches the same
// pattern already proven working for libmfc.so (see tools/hx170-test/
// hx170-test.c and androidauto/video_channel.h): dlopen a real,
// already-present target .so at runtime rather than requiring it at
// link time. This sidesteps the whole static-linking/glibc-mismatch
// question entirely -- the target's own libasound.so.2.0.0 was built
// against the target's own glibc 2.27, and once loaded via the
// target's own runtime dynamic linker (not our cross toolchain's), it
// resolves its own dependencies normally.
//
// The ALSA snd_pcm_* API used here is 100% public, standard, stable
// ABI (unlike libmfc.so's proprietary Hantro wrapper) -- the function
// signatures and enum values below are not reverse-engineered, they're
// from ALSA's own long-stable public headers. Only the dlopen path and
// exact PCM device string are project-specific/confirmed.
//
// PCM device strings are real, confirmed values -- NOT guessed --
// cross-referenced from docs/AUDIO_SUBSYSTEM_INVESTIGATION.md's fully
// closed investigation into ArkMediaPlayer::setup()/IUserLinkPlayer's
// mode->device mapping (independently confirmed via two separate
// decompiles): music/media -> "plug:softvol2", TTS/guidance ->
// "plug:softvol1", VR -> "plug:softvol4", calls -> "plug:softvol3".
// Android Auto's SYSTEM_AUDIO channel doesn't have a confirmed 1:1
// stock equivalent -- "plug:softvol4" (VR) is used as the closest
// reasonable match (both are secondary/interjecting audio, not the
// main media stream), flagged as an approximation, not a confirmed
// mapping.
//
// NOT YET hardware-tested.
#pragma once

#include <cstdint>
#include <string>

namespace androidauto {

class AlsaOutput {
public:
    // deviceName: a real confirmed PCM device string, see header
    // comment (e.g. "plug:softvol2" for media audio).
    AlsaOutput(std::string deviceName, uint32_t sampleRate, uint32_t bitsPerSample,
               uint32_t channels);
    ~AlsaOutput();

    AlsaOutput(const AlsaOutput &) = delete;
    AlsaOutput & operator=(const AlsaOutput &) = delete;

    // dlopen's libasound, opens the PCM device, and configures it per
    // the constructor's sample-format parameters. Returns false (logs
    // the reason) on any failure -- non-fatal, matches this codebase's
    // general "missing/failed optional hardware isn't fatal" pattern.
    bool open();

    // Writes interleaved PCM samples. Returns false on an
    // unrecoverable write error (after one snd_pcm_recover() retry,
    // matching ALSA's own documented XRUN-recovery pattern) -- frameCount
    // is in frames (samples per channel), not bytes.
    bool write(const void * interleavedSamples, uint32_t frameCount);

    void close();

private:
    std::string deviceName_;
    uint32_t sampleRate_;
    uint32_t bitsPerSample_;
    uint32_t channels_;

    void * lib_ = nullptr;
    void * pcmHandle_ = nullptr;

    // Resolved via dlsym in open() -- see alsa_output.cpp for the
    // exact public ALSA signatures.
    int (*snd_pcm_open_)(void **, const char *, int, int) = nullptr;
    int (*snd_pcm_set_params_)(void *, int, int, unsigned int, unsigned int, int,
                                unsigned int) = nullptr;
    long (*snd_pcm_writei_)(void *, const void *, unsigned long) = nullptr;
    int (*snd_pcm_recover_)(void *, int, int) = nullptr;
    int (*snd_pcm_close_)(void *) = nullptr;
    const char * (*snd_strerror_)(int) = nullptr;
};

}  // namespace androidauto
