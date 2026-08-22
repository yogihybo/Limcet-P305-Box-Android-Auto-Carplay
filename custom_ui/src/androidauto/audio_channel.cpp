#include "androidauto/audio_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"

namespace androidauto {

namespace {
// 2026-08-19: per docs/AUDIO_SUBSYSTEM_HANDOFF.md's adaptive high-
// water-mark design -- below this many buffers queued in AlsaOutput,
// acks fire immediately on receipt (keeps the network pipeline
// saturated during normal playback); at/above it, acks defer to real
// playback completion (AlsaOutput's consumed callback), pacing the
// phone back down to real-time instead of letting a pre-buffer burst
// keep piling into the queue. Same value as the old kMaxQueuedBuffers
// cap this replaces as the thing that actually prevents overflow --
// the queue itself is now sized much larger (256, see alsa_output.h)
// purely as a backstop, not the primary defense.
constexpr size_t kHighWaterMarkBuffers = 32;
}  // namespace

AudioChannel::AudioChannel(boost::asio::io_service::strand & strand,
                           aasdk::messenger::IMessenger::Pointer messenger,
                           aasdk::messenger::ChannelId channelId, std::string pcmDevice,
                           uint32_t sampleRate, uint32_t channels)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::mediasink::audio::AudioMediaSinkService>(
          strand, std::move(messenger), channelId)),
      pcmDevice_(std::move(pcmDevice)),
      sampleRate_(sampleRate),
      channels_(channels),
      alsaOutput_(pcmDevice_, sampleRate_, 16, channels_) {
}

void AudioChannel::start() {
    // 2026-08-19: self-capturing shared_ptr, not a bare `this` -- see
    // alsa_output.h's own class comment for the full history.
    // AlsaOutput invokes this from ITS OWN writer thread, so the
    // pendingPacedAcks_ load below must stay a relaxed atomic read,
    // not a strand-only access -- the whole point is deciding whether
    // to post at all WITHOUT already being on strand_. Only actually
    // posts (and only then does strand_ work: decrement + sendAck())
    // when there's a real paced ack owed, so this costs nothing during
    // normal below-high-water-mark playback beyond one atomic load per
    // write.
    auto self = shared_from_this();
    alsaOutput_.setConsumedCallback([this, self]() {
        if (pendingPacedAcks_.load(std::memory_order_relaxed) == 0) {
            return;
        }
        strand_.post([this, self]() {
            if (pendingPacedAcks_ > 0) {
                --pendingPacedAcks_;
                sendAck();
            }
        });
    });
    channel_->receive(this->shared_from_this());
}

void AudioChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest & request) {
    std::printf("%s androidauto: audio channel (%s) open request (priority=%d)\n", logTimestamp().c_str(),
               pcmDevice_.c_str(), request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this]() {
            std::printf("%s androidauto: audio channel (%s) open response sent\n", logTimestamp().c_str(),
                        pcmDevice_.c_str());
        },
        [](const aasdk::error::Error & e) {
            std::printf("%s androidauto: audio channel open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup & request) {
    std::printf("%s androidauto: audio channel (%s) setup request, codec type=%d\n", logTimestamp().c_str(),
               pcmDevice_.c_str(), static_cast<int>(request.type()));

    // Only one configuration is ever advertised for this channel (see
    // Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    // 2026-08-15: found via a real phone-side adb logcat capture --
    // Gearhead rejected the session with "Critical error 2 detail: 39
    // msg: MaxUnacked must be >= 0, was 0" (fired once per AudioChannel
    // instance -- media/system/speech, matching this class being
    // constructed 3 times), immediately followed by "Failed to read
    // message" and teardown. max_unacked (Config.proto field 2) is
    // optional but apparently required in practice.
    //
    // 2026-08-19: raised from 1 to 8 per docs/AUDIO_SUBSYSTEM_HANDOFF.md
    // -- with max_unacked=1 the ack IS the flow control (phone can't
    // send buffer N+1 until N is acked), so acking at real playback
    // pace (the previous fix, see git history) turns the network
    // round-trip into an unavoidable per-buffer silence gap. Stock's
    // own libAndroidAuto.so (decompile-confirmed,
    // docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md's "RESOLVED: audio
    // dispatch is genuinely asynchronous" section) acks immediately on
    // receipt and relies on a wider window plus a decoupled playback
    // queue to smooth network jitter instead. 8 is not itself a
    // decompile-confirmed value (the decompile confirmed immediate
    // delta-ack, not this specific window size) -- treat as a tunable
    // if stutter persists.
    response.set_max_unacked(8);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this]() {
            std::printf("%s androidauto: audio channel (%s) setup response sent\n", logTimestamp().c_str(),
                        pcmDevice_.c_str());
        },
        [](const aasdk::error::Error & e) {
            std::printf("%s androidauto: audio channel setup response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start & indication) {
    sessionId_ = indication.session_id();
    std::printf("%s androidauto: audio channel (%s) start, session_id=%d config_index=%u\n", logTimestamp().c_str(),
               pcmDevice_.c_str(), sessionId_, indication.configuration_index());

    if (!alsaOpen_) {
        alsaOpen_ = alsaOutput_.open();
        if (!alsaOpen_) {
            std::printf("%s androidauto: audio channel (%s) ALSA open failed -- audio for this "
                       "channel won't play\n", logTimestamp().c_str(), pcmDevice_.c_str());
        }
    }
    // 2026-08-21: proactively reset the PCM's own hardware state on
    // every Start (not just the first one) -- see AlsaOutput::prepare()'s
    // own comment for why: onMediaChannelStopIndication()'s flush() only
    // ever clears the software queue_, so a real XRUN from the PCM
    // sitting idle during a Stop gap was previously left for the first
    // post-Start write() to discover reactively, under load, inside
    // writeBlocking()'s recovery loop. Doing it here instead, before any
    // audio data has arrived, is what actually prevents the start/stop/
    // restart-correlated crash rather than just bounding its failure
    // mode (that bound is real too, see writeBlocking()'s own comment,
    // but this is the fix that stops it from needing to trigger at all
    // on an ordinary restart).
    if (alsaOpen_) {
        alsaOutput_.prepare();
    }

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop &) {
    std::printf("%s androidauto: audio channel (%s) stop\n", logTimestamp().c_str(), pcmDevice_.c_str());

    // 2026-08-19: Start/Stop fire repeatedly within a single AA session
    // (not just once at connect/disconnect), so without this a stale
    // queued backlog -- or a pendingPacedAcks_ count left over from
    // backpressure right before Stop arrived -- could bleed into the
    // next Start. See alsa_output.h's flush() for why this doesn't
    // touch the PCM device itself.
    if (alsaOpen_) {
        alsaOutput_.flush();
    }
    pendingPacedAcks_.store(0, std::memory_order_relaxed);

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType,
                                                   const aasdk::common::DataConstBuffer & buffer) {
    playBuffer(buffer);
    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaIndication(const aasdk::common::DataConstBuffer & buffer) {
    playBuffer(buffer);
    channel_->receive(this->shared_from_this());
}

void AudioChannel::playBuffer(const aasdk::common::DataConstBuffer & buffer) {
    if (alsaOpen_) {
        // 16-bit samples, channels_ interleaved channels per frame.
        uint32_t bytesPerFrame = 2 * channels_;
        uint32_t frameCount = static_cast<uint32_t>(buffer.size / bytesPerFrame);
        if (frameCount > 0) {
            alsaOutput_.write(buffer.cdata, frameCount);

            // Adaptive high-water-mark pacing -- see alsa_output.h's
            // own class comment for the full history. Below
            // kHighWaterMarkBuffers queued, keep acking immediately
            // (network pipeline stays saturated during normal
            // playback); at/above it, defer this buffer's ack to
            // AlsaOutput's consumed callback instead (see start()'s
            // own comment), so the phone's own max_unacked window
            // naturally throttles it back to real playback speed
            // instead of continuing to race ahead into a growing
            // backlog.
            if (alsaOutput_.queuedBuffers() >= kHighWaterMarkBuffers) {
                ++pendingPacedAcks_;
                return;
            }
        }
    }
    sendAck();
}

void AudioChannel::sendAck() {
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(sessionId_);
    ack.set_ack(1);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    channel_->sendMediaAckIndication(ack, promise);
}

void AudioChannel::onChannelError(const aasdk::error::Error & e) {
    std::printf("%s androidauto: audio channel (%s) error: %s\n", logTimestamp().c_str(), pcmDevice_.c_str(),
               e.what());
}

}  // namespace androidauto
