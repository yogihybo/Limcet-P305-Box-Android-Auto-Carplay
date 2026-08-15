// Minimal ALSA PCM capture wrapper, statically linked against our own
// cross-compiled alsa-lib -- same build/config-path setup as
// AlsaOutput (see alsa_output.h's own header comment), just the
// capture direction.
//
// 2026-08-15: device string is "plug:hw:0,0" (card 0, device 0 --
// this device's real, single I2S/SDADC capture path), not routed
// through dmix/dsnoop like AlsaOutput's playback devices -- dmix only
// applies to playback (mixing multiple writers into one hardware
// device), and dsnoop (its capture-sharing equivalent) isn't needed
// here since this is the only capturer. "plug:" (not raw "hw:0,0")
// deliberately kept for the same reason AlsaOutput uses it: lets
// ALSA's own rate/format plugins absorb a mismatch between this
// device's real native capture format and Android Auto's requested
// 16kHz/16-bit/mono (see session.cpp's ServiceDiscoveryResponse) if
// the hardware doesn't support that combination directly, rather than
// snd_pcm_open() failing outright on an exact-match requirement --
// unlike stock's own `sink` binary, which is confirmed (see
// project_mic_capture_investigation memory) to open raw "hw:0,0", but
// stock's own AA implementation controls its own requested format and
// isn't relevant here.
//
// The underlying kernel capture path (SARADC settle-delay fix,
// linux-arkmicro 4a1d8a213) and userspace unblocking condition
// (MsnProductInfo.ini's SoundType, which only gates libMsnSound.so's
// own init path, not raw ALSA) are both already covered by
// project_mic_capture_investigation's hardware-confirmed findings --
// this class only needs to open the same underlying ALSA capture
// device those findings already proved works, not repeat that
// investigation.
//
// NOT YET hardware-tested.
#pragma once

#include <cstdint>
#include <string>

#include <alsa/asoundlib.h>

namespace androidauto {

class AlsaInput {
public:
    AlsaInput(std::string deviceName, uint32_t sampleRate, uint32_t bitsPerSample, uint32_t channels);
    ~AlsaInput();

    AlsaInput(const AlsaInput &) = delete;
    AlsaInput & operator=(const AlsaInput &) = delete;

    // Opens the PCM device and configures it per the constructor's
    // sample-format parameters. Returns false (logs the reason) on any
    // failure -- non-fatal, matches AlsaOutput's own pattern.
    bool open();

    // Reads up to `frameCount` interleaved PCM frames into `outBuffer`
    // (must be at least frameCount * channels * (bitsPerSample/8)
    // bytes). Returns the number of frames actually read, or -1 on an
    // unrecoverable error (after one snd_pcm_recover() retry, mirroring
    // AlsaOutput::write()'s own XRUN-recovery pattern). Blocks until at
    // least one frame is available (this class doesn't configure
    // SND_PCM_NONBLOCK) -- callers must run this on their own dedicated
    // thread, never the LVGL/aasdk strand thread.
    int read(void * outBuffer, uint32_t frameCount);

    void close();

private:
    std::string deviceName_;
    uint32_t sampleRate_;
    uint32_t bitsPerSample_;
    uint32_t channels_;

    snd_pcm_t * pcmHandle_ = nullptr;
};

}  // namespace androidauto
