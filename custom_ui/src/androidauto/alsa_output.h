// Minimal ALSA PCM playback wrapper, statically linked against our own
// cross-compiled alsa-lib (see custom_ui/Makefile's ALSA_ARM_INSTALL) --
// NOT dlopen'd against the target rootfs's libasound.so.2.0.0 anymore.
//
// 2026-08-15 REVISED: this used to dlopen the target's own
// libasound.so.2.0.0 at runtime (same pattern as libmfc.so) specifically
// to avoid a static-linking/glibc-mismatch concern -- but that dlopen
// call itself was one of the last remaining triggers for this binary's
// glibc static-NSS-init startup crash (see hantro_dlopen.c's own header
// comment for the full story). Cross-compiled alsa-lib 1.2.12 statically
// with `--with-libdl=no` instead, which builds every PCM/control plugin
// this device's real /etc/asound.conf needs (dmix, dsnoop, softvol,
// plug, rate, hw) directly into libasound.a rather than as separate
// dlopen'd modules -- confirmed via `nm` that the resulting libasound.a
// references no dlopen/dlsym/dlclose symbols at all. This closes the
// last dlopen gap outside of the one intentional, already-handled case
// (libmfc.so, via hantro_dlopen.c's own minimal loader).
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

#include <alsa/asoundlib.h>

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

    // Opens the PCM device and configures it per the constructor's
    // sample-format parameters. Returns false (logs the reason) on any
    // failure -- non-fatal, matches this codebase's general "missing/
    // failed optional hardware isn't fatal" pattern.
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

    snd_pcm_t * pcmHandle_ = nullptr;
};

}  // namespace androidauto
