#include "androidauto/audio_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"

namespace androidauto {

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
    // 2026-08-18: see alsa_output.h's own class comment for the full
    // story -- this channel's ack is max_unacked=1's actual flow
    // control (the phone won't send the next buffer until it gets
    // this), so it must fire at real playback pace, not the instant a
    // buffer is handed to AlsaOutput::write() (which now just enqueues
    // it). AlsaOutput invokes this from ITS OWN writer thread (or
    // synchronously from write() on the drop path) -- never assume
    // it's already on strand_, always post.
    alsaOutput_.setConsumedCallback([this]() {
        strand_.post([this]() { sendAck(); });
    });
}

void AudioChannel::start() {
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
    // optional but apparently required in practice. 1 matches
    // microphone_channel.cpp's own already-correct value, which itself
    // matches the real upstream f1x/openauto reference.
    response.set_max_unacked(1);
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

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop &) {
    std::printf("%s androidauto: audio channel (%s) stop\n", logTimestamp().c_str(), pcmDevice_.c_str());
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
            // Ack fires later, via the consumed-callback set in the
            // constructor -- once this buffer's real write actually
            // happens (or immediately if dropped) -- not here. See
            // alsa_output.h's class comment: acking immediately after
            // just enqueueing broke max_unacked=1's flow control and
            // crashed the session on real hardware.
            alsaOutput_.write(buffer.cdata, frameCount);
            return;
        }
    }
    // ALSA never opened, or a zero-length buffer -- nothing will ever
    // invoke the consumed callback for this one, so ack directly.
    sendAck();
}

void AudioChannel::sendAck() {
    ++ackCount_;
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(sessionId_);
    ack.set_ack(static_cast<uint32_t>(ackCount_));

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: audio ack send failed: %s\n", e.what());
        });
    channel_->sendMediaAckIndication(ack, promise);
}

void AudioChannel::onChannelError(const aasdk::error::Error & e) {
    std::printf("%s androidauto: audio channel (%s) error: %s\n", logTimestamp().c_str(), pcmDevice_.c_str(),
               e.what());
}

}  // namespace androidauto
