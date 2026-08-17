#include "androidauto/alsa_output.h"
#include "androidauto/log_timing.h"

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
        std::fprintf(stderr, "%s androidauto::AlsaOutput: only 16-bit PCM supported, got %u\n", androidauto::logTimestamp().c_str(),
                     bitsPerSample_);
        return false;
    }

    int err = snd_pcm_open(&pcmHandle_, deviceName_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::fprintf(stderr, "%s androidauto::AlsaOutput: snd_pcm_open(%s) failed: %s\n", androidauto::logTimestamp().c_str(),
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
        std::fprintf(stderr, "%s androidauto::AlsaOutput: snd_pcm_set_params(%s) failed: %s\n", androidauto::logTimestamp().c_str(),
                     deviceName_.c_str(), snd_strerror(err));
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
        return false;
    }

    std::printf("%s androidauto::AlsaOutput: opened %s (%u Hz, 16-bit, %u ch)\n", androidauto::logTimestamp().c_str(), deviceName_.c_str(),
               sampleRate_, channels_);

    stop_ = false;
    writerThread_ = std::thread(&AlsaOutput::writerLoop, this);
    return true;
}

bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
    if (!pcmHandle_) return false;

    uint32_t bytesPerFrame = 2 * channels_;  // 16-bit samples, see header comment
    const uint8_t * bytes = static_cast<const uint8_t *>(interleavedSamples);
    std::vector<uint8_t> copy(bytes, bytes + static_cast<size_t>(frameCount) * bytesPerFrame);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueuedBuffers) {
            ++droppedBuffers_;
            if (droppedBuffers_ == 1 || droppedBuffers_ % 100 == 0) {
                std::fprintf(stderr, "%s androidauto::AlsaOutput: writer thread for %s falling behind, "
                             "dropped %u buffer(s) so far\n", androidauto::logTimestamp().c_str(),
                             deviceName_.c_str(), droppedBuffers_);
            }
            // Still counts as "consumed" -- see class comment: acking a
            // dropped buffer immediately is correct, since there's
            // nothing left to wait for and NOT acking would stall the
            // phone forever expecting a buffer we've already discarded.
            if (onConsumed_) onConsumed_();
            return true;
        }
        queue_.push_back(std::move(copy));
    }
    cv_.notify_one();
    return true;
}

void AlsaOutput::writerLoop() {
    for (;;) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_) return;
                continue;
            }
            buf = std::move(queue_.front());
            queue_.pop_front();
        }
        uint32_t bytesPerFrame = 2 * channels_;
        uint32_t frameCount = static_cast<uint32_t>(buf.size() / bytesPerFrame);
        if (frameCount > 0) {
            // Blocking, real-time-paced -- see class comment: this is
            // exactly what restores max_unacked=1's intended flow
            // control once onConsumed_ is wired to the ack below.
            writeBlocking(buf.data(), frameCount);
        }
        if (onConsumed_) onConsumed_();
    }
}

bool AlsaOutput::writeBlocking(const void * interleavedSamples, uint32_t frameCount) {
    snd_pcm_sframes_t written = snd_pcm_writei(pcmHandle_, interleavedSamples, frameCount);
    if (written < 0) {
        // ALSA's own documented XRUN-recovery pattern: retry once via
        // snd_pcm_recover(), then re-attempt the write.
        int recovered = snd_pcm_recover(pcmHandle_, static_cast<int>(written), 1);
        if (recovered < 0) {
            std::fprintf(stderr, "%s androidauto::AlsaOutput: unrecoverable write error on %s: %s\n", androidauto::logTimestamp().c_str(),
                         deviceName_.c_str(), snd_strerror(recovered));
            return false;
        }
        written = snd_pcm_writei(pcmHandle_, interleavedSamples, frameCount);
        if (written < 0) {
            std::fprintf(stderr, "%s androidauto::AlsaOutput: write failed even after recovery on "
                         "%s: %s\n", androidauto::logTimestamp().c_str(), deviceName_.c_str(), snd_strerror(static_cast<int>(written)));
            return false;
        }
    }
    return true;
}

void AlsaOutput::close() {
    if (writerThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        writerThread_.join();
    }
    if (pcmHandle_) {
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }
}

}  // namespace androidauto
