#include "androidauto/microphone_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"

#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>

namespace androidauto {

MicrophoneChannel::MicrophoneChannel(boost::asio::io_service::strand &strand,
                                      aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
          strand, std::move(messenger), aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE)) {
}

void MicrophoneChannel::start() {
    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("[+%ldms] androidauto: microphone channel open request (priority=%d)\n", elapsedMs(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: microphone channel open response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: microphone channel open response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup &request) {
    std::printf("[+%ldms] androidauto: microphone channel setup request, codec type=%d\n", elapsedMs(),
                static_cast<int>(request.type()));

    // Only one configuration is ever advertised for this channel (see
    // Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: microphone channel setup response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: microphone channel setup response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onMediaSourceOpenRequest(
    const aap_protobuf::service::media::source::message::MicrophoneRequest &request) {
    // Structural-only: acknowledges open/close correctly (the phone
    // needs a real MicrophoneResponse to consider this channel usable
    // at all) but doesn't actually capture or stream microphone audio
    // -- see this class's header comment for why that's fine for now.
    std::printf("[+%ldms] androidauto: microphone %s request\n", elapsedMs(),
                request.open() ? "open" : "close");

    aap_protobuf::service::media::source::message::MicrophoneResponse response;
    response.set_status(static_cast<std::int32_t>(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS));
    response.set_session_id(sessionId_);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: microphone open response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: microphone open response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendMicrophoneOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onMediaChannelAckIndication(
    const aap_protobuf::service::media::source::message::Ack &) {
    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("[+%ldms] androidauto: microphone channel error: %s\n", elapsedMs(), e.what());
}

}  // namespace androidauto
