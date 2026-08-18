#include "androidauto/alsa_output.h"
#include "androidauto/log_timing.h"

#include <cstdio>
#include <cstring>

#include <pthread.h>

namespace androidauto {

namespace {

// 2026-08-18: real hardware showed irregular audio stutter -- no
// queue drops (kMaxQueuedBuffers never hit) and only a single genuine
// ALSA XRUN in dmesg across long sessions, yet audible glitches with
// no clear trigger, sometimes 10+s of clean playback in between. That
// pattern doesn't match a supply/protocol issue (which would show up
// as queue drops or repeated XRUNs) -- it matches a writer thread
// with no real-time scheduling guarantee competing for CPU on equal
// footing with everything else (video decode, LVGL rendering in
// custom_ui, aasdk message processing) under plain SCHED_OTHER. A few
// milliseconds of scheduling delay right when ALSA's ring buffer
// needs feeding is enough to produce an audible glitch without ever
// triggering a hard, log-worthy XRUN. Standard fix: SCHED_FIFO with a
// modest real-time priority, so the kernel guarantees this thread CPU
// time ahead of normal-priority work whenever it actually needs to
// run, rather than waiting its turn. Requires CAP_SYS_NICE or root --
// this device runs everything as root, but failure is still handled
// as non-fatal (falls back to whatever the default policy already
// was) matching this codebase's general optional-hardware pattern.
void raiseToRealtimePriority(std::thread & thread, const char * deviceName) {
    sched_param param{};
    // Modest, not maximal -- well below anything kernel-critical,
    // just enough to preempt normal SCHED_OTHER work (everything else
    // in this process and custom_ui) when this thread has real work
    // to do. Real-time priority range on Linux is typically 1-99;
    // this deliberately stays low in that range.
    param.sched_priority = 10;
    int rc = pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &param);
    if (rc != 0) {
        std::fprintf(stderr, "%s androidauto::AlsaOutput: couldn't raise writer thread for %s to "
                     "SCHED_FIFO (%s) -- continuing at default scheduling priority\n",
                     androidauto::logTimestamp().c_str(), deviceName, std::strerror(rc));
    }
}

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
    raiseToRealtimePriority(writerThread_, deviceName_.c_str());
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
            // 2026-08-19: drop the OLDEST queued buffer, not the new
            // one -- acks are no longer gated on real playback (see
            // audio_channel.cpp's sendAck(), now fired immediately on
            // receipt matching stock's confirmed behavior), so this
            // queue's only job is to track real-time stream state.
            // Keeping stale audio and dropping the newest would just
            // grow playback latency under sustained backlog instead of
            // catching back up.
            queue_.pop_front();
            ++droppedBuffers_;
            if (droppedBuffers_ == 1 || droppedBuffers_ % 100 == 0) {
                std::fprintf(stderr, "%s androidauto::AlsaOutput: writer thread for %s falling behind, "
                             "dropped %u buffer(s) so far\n", androidauto::logTimestamp().c_str(),
                             deviceName_.c_str(), droppedBuffers_);
            }
        }
        queue_.push_back(std::move(copy));
    }
    cv_.notify_one();
    return true;
}

void AlsaOutput::writerLoop() {
    uint32_t consecutiveErrors = 0;
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
            bool ok = writeBlocking(buf.data(), frameCount);
            if (!ok) {
                ++consecutiveErrors;
                // Guard against tight CPU spinning when ALSA fails on this single-core SoC:
                // writeBlocking failed unrecoverably, so yield the CPU for 20ms. This ensures
                // the real-time writer thread never starves custom_ui and LVGL rendering.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

                if (consecutiveErrors >= 5) {
                    // Sustained audio hardware error: clear stale backlog to prevent queue churn
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.clear();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } else {
                consecutiveErrors = 0;
            }
        }
    }
}

bool AlsaOutput::writeBlocking(const void * interleavedSamples, uint32_t frameCount) {
    // 2026-08-19: real gap found via code audit -- snd_pcm_writei() is
    // documented to be able to return 0 < written < frameCount even in
    // blocking mode (a genuine short write, not an error -- ALSA's own
    // API contract, e.g. on a signal interrupting the underlying
    // syscall). This function used to check only `written < 0`
    // (hard error) and otherwise return success unconditionally,
    // silently dropping whatever fraction of the buffer wasn't
    // actually written -- a real, if intermittent, source of audio
    // clicks/pops. Loops until every frame is written (or a genuinely
    // unrecoverable error occurs), advancing the buffer pointer by
    // however many frames each call actually consumed.
    const uint8_t * cursor = static_cast<const uint8_t *>(interleavedSamples);
    uint32_t bytesPerFrame = 2 * channels_;  // 16-bit samples, see header comment
    uint32_t remaining = frameCount;
    while (remaining > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcmHandle_, cursor, remaining);
        if (written < 0) {
            // ALSA's own documented XRUN-recovery pattern: retry once
            // via snd_pcm_recover(), then re-attempt the same
            // remaining frames (not the original full buffer -- some
            // of it may have already been written in an earlier loop
            // iteration).
            int recovered = snd_pcm_recover(pcmHandle_, static_cast<int>(written), 1);
            if (recovered < 0) {
                std::fprintf(stderr, "%s androidauto::AlsaOutput: unrecoverable write error on %s: %s\n", androidauto::logTimestamp().c_str(),
                             deviceName_.c_str(), snd_strerror(recovered));
                return false;
            }
            continue;
        }
        if (written == 0) {
            // Shouldn't happen for a successful (non-negative) return,
            // but guard against ever looping forever if it somehow
            // does.
            std::fprintf(stderr, "%s androidauto::AlsaOutput: snd_pcm_writei() on %s returned 0 with %u "
                         "frames remaining -- giving up rather than spinning\n", androidauto::logTimestamp().c_str(),
                         deviceName_.c_str(), remaining);
            return false;
        }
        cursor += static_cast<size_t>(written) * bytesPerFrame;
        remaining -= static_cast<uint32_t>(written);
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
