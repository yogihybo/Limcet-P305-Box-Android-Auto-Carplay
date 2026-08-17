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

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <alsa/asoundlib.h>

namespace androidauto {

// 2026-08-17: write() used to call snd_pcm_writei() directly, blocking,
// on whatever thread called it -- for every AudioChannel (media/
// guidance/system, see audio_channel.h) that's this project's own
// single shared io_service thread (see
// wireless_session_manager.cpp/session.cpp's ioService.run() -- ONE
// thread services every channel: video decode dispatch, all three
// audio channels, sensor, input, control, all of it). snd_pcm_open()
// here is blocking mode (no SND_PCM_NONBLOCK), and snd_pcm_writei()
// blocks until the ALSA ring buffer has room -- with a 200ms buffer
// (see open()'s own comment) that's tens of milliseconds the ENTIRE
// session's message pump can't run at all, every time a write happens
// to land against a fuller-than-usual buffer. That's a direct,
// concrete explanation for real-hardware "stutter and dropped frames"
// on video specifically -- unrelated to how much total CPU time this
// process uses, purely a single-thread head-of-line blocking problem.
//
// Fixed by moving the actual blocking snd_pcm_writei() call onto a
// dedicated writer thread per AlsaOutput instance: write() now just
// copies the caller's buffer into a small queue and returns
// immediately, so the shared io_service thread is never blocked on
// ALSA. Queue is capped (see kMaxQueuedBuffers) -- if the writer
// thread ever genuinely falls behind (real system overload), new
// audio is dropped rather than growing an unbounded backlog that
// would just turn into ever-increasing playback latency.
//
// 2026-08-18: real hardware showed CPU drop dramatically (as
// expected) but the AA session itself now crashes with ECONNRESET a
// few seconds in, after audibly stuttery playback. Root cause: the
// FIRST version of this fix left AudioChannel::playBuffer() calling
// sendAck() immediately after handing a buffer to write() -- but
// write() now only enqueues; it doesn't wait for the buffer to
// actually be played. With max_unacked=1 (see
// AudioChannel::onMediaChannelSetupRequest's own comment), the ack is
// literally the phone's permission to send the next buffer -- acking
// the instant a buffer is queued, rather than once it's actually
// drained at real playback speed, let the phone burst audio at
// network speed instead of real-time, across all three audio channels
// simultaneously, overrunning this queue (hence the audible stutter)
// and very plausibly overrunning something in the transport itself
// (ECONNRESET/Native Code 104 shortly after). The blocking write this
// class replaced was, unintentionally, also the thing pacing acks to
// real playback speed.
//
// Fixed by adding an optional "consumed" callback, invoked once per
// buffer -- from the writer thread, right after that buffer's real
// (blocking, real-time-paced) write attempt, OR synchronously from
// write() itself on the rare drop path (queue full -- acking
// immediately there is correct: we're discarding it, so there's
// nothing to wait for, and NOT acking would stall the phone
// indefinitely waiting for a buffer that's never coming). The
// callback fires on whichever thread the event happened on -- the
// caller (AudioChannel) is responsible for marshaling back onto its
// own strand (strand_.post(...)) before touching any aasdk state, the
// same pattern microphone_channel.cpp's own capture thread already
// uses for the equivalent capture-side problem.
class AlsaOutput {
public:
    // deviceName: a real confirmed PCM device string, see header
    // comment (e.g. "plug:softvol2" for media audio).
    AlsaOutput(std::string deviceName, uint32_t sampleRate, uint32_t bitsPerSample,
               uint32_t channels);
    ~AlsaOutput();

    AlsaOutput(const AlsaOutput &) = delete;
    AlsaOutput & operator=(const AlsaOutput &) = delete;

    // Opens the PCM device (per the constructor's sample-format
    // parameters) and starts the background writer thread. Returns
    // false (logs the reason) on any failure -- non-fatal, matches
    // this codebase's general "missing/failed optional hardware isn't
    // fatal" pattern.
    bool open();

    // Enqueues interleaved PCM samples for the writer thread and
    // returns immediately -- never blocks on ALSA itself. frameCount
    // is in frames (samples per channel), not bytes. Always returns
    // true once open() succeeded; a full queue silently drops the
    // buffer (logged, rate-limited) rather than blocking or growing
    // without bound -- see class comment.
    bool write(const void * interleavedSamples, uint32_t frameCount);

    // Invoked once per buffer passed to write() -- after its real
    // write attempt completes on the writer thread, or synchronously
    // within write() itself if the buffer was dropped (queue full).
    // See class comment. Not thread-safe to change after open(); set
    // once, before the first write().
    using ConsumedCallback = std::function<void()>;
    void setConsumedCallback(ConsumedCallback cb) { onConsumed_ = std::move(cb); }

    // Stops the writer thread (letting it drain whatever's already
    // queued) and closes the PCM device.
    void close();

private:
    void writerLoop();
    // The actual blocking snd_pcm_writei() + XRUN-recovery call,
    // moved out of write() onto writerLoop()'s own thread.
    bool writeBlocking(const void * interleavedSamples, uint32_t frameCount);

    std::string deviceName_;
    uint32_t sampleRate_;
    uint32_t bitsPerSample_;
    uint32_t channels_;

    snd_pcm_t * pcmHandle_ = nullptr;

    // Caps memory/latency growth if the writer thread ever falls
    // behind -- see class comment. ~1s of audio at a typical AA
    // buffer size (a few dozen ms per buffer), generous enough to
    // absorb normal scheduling jitter without masking a real,
    // sustained problem.
    static constexpr size_t kMaxQueuedBuffers = 32;

    std::thread writerThread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    bool stop_ = false;
    uint32_t droppedBuffers_ = 0;
    ConsumedCallback onConsumed_;
};

}  // namespace androidauto
