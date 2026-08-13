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
      alsaOutput_(pcmDevice_, sampleRate_, 16, channels_) {}

void AudioChannel::start() {
    channel_->receive(this->shared_from_this());
}

void AudioChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest & request) {
    std::printf("[+%ldms] androidauto: audio channel (%s) open request (priority=%d)\n", elapsedMs(),
               pcmDevice_.c_str(), request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this]() {
            std::printf("[+%ldms] androidauto: audio channel (%s) open response sent\n", elapsedMs(),
                        pcmDevice_.c_str());
        },
        [](const aasdk::error::Error & e) {
            std::printf("[+%ldms] androidauto: audio channel open response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup & request) {
    std::printf("[+%ldms] androidauto: audio channel (%s) setup request, codec type=%d\n", elapsedMs(),
               pcmDevice_.c_str(), static_cast<int>(request.type()));

    // Only one configuration is ever advertised for this channel (see
    // Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this]() {
            std::printf("[+%ldms] androidauto: audio channel (%s) setup response sent\n", elapsedMs(),
                        pcmDevice_.c_str());
        },
        [](const aasdk::error::Error & e) {
            std::printf("[+%ldms] androidauto: audio channel setup response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start & indication) {
    sessionId_ = indication.session_id();
    std::printf("[+%ldms] androidauto: audio channel (%s) start, session_id=%d config_index=%u\n", elapsedMs(),
               pcmDevice_.c_str(), sessionId_, indication.configuration_index());

    if (!alsaOpen_) {
        alsaOpen_ = alsaOutput_.open();
        if (!alsaOpen_) {
            std::printf("[+%ldms] androidauto: audio channel (%s) ALSA open failed -- audio for this "
                       "channel won't play\n", elapsedMs(), pcmDevice_.c_str());
        }
    }

    channel_->receive(this->shared_from_this());
}

void AudioChannel::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop &) {
    std::printf("[+%ldms] androidauto: audio channel (%s) stop\n", elapsedMs(), pcmDevice_.c_str());
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
        }
    }
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
    std::printf("[+%ldms] androidauto: audio channel (%s) error: %s\n", elapsedMs(), pcmDevice_.c_str(),
               e.what());
}

}  // namespace androidauto
