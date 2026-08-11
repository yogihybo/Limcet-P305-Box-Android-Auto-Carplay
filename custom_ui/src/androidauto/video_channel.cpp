#include "androidauto/video_channel.h"

#include <cstdio>

namespace androidauto {

VideoChannel::VideoChannel(boost::asio::io_service::strand & strand,
                           aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::mediasink::video::VideoMediaSinkService>(
          strand, std::move(messenger), aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO)) {}

void VideoChannel::start() {
    channel_->receive(this->shared_from_this());
}

void VideoChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest & request) {
    std::printf("androidauto: video channel open request (priority=%d)\n", request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: video channel open response send failed: %s\n", e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup & request) {
    std::printf("androidauto: video channel setup request, codec type=%d\n",
               static_cast<int>(request.type()));

    // Only one configuration is ever advertised (VIDEO_800x480 H264_BP,
    // see Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: video channel setup response send failed: %s\n", e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start & indication) {
    sessionId_ = indication.session_id();
    std::printf("androidauto: video channel start, session_id=%d config_index=%u\n", sessionId_,
               indication.configuration_index());

    if (!decoderOpen_) {
        decoderOpen_ = decoder_.open();
        if (!decoderOpen_) {
            std::printf("androidauto: video decoder open failed -- no video for this session\n");
        }
    }

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop &) {
    std::printf("androidauto: video channel stop\n");
    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType,
                                                   const aasdk::common::DataConstBuffer & buffer) {
    decodeBuffer(buffer);
    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaIndication(const aasdk::common::DataConstBuffer & buffer) {
    decodeBuffer(buffer);
    channel_->receive(this->shared_from_this());
}

void VideoChannel::decodeBuffer(const aasdk::common::DataConstBuffer & buffer) {
    if (decoderOpen_) {
        decoder_.decodeFrame(buffer.cdata, buffer.size);
    }
    sendAck();
}

void VideoChannel::sendAck() {
    ++ackCount_;
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(sessionId_);
    ack.set_ack(static_cast<uint32_t>(ackCount_));

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: video ack send failed: %s\n", e.what());
        });
    channel_->sendMediaAckIndication(ack, promise);
}

void VideoChannel::onVideoFocusRequest(
    const aap_protobuf::service::media::video::message::VideoFocusRequestNotification & request) {
    std::printf("androidauto: video focus request, mode=%d reason=%d\n",
               static_cast<int>(request.mode()), static_cast<int>(request.reason()));

    // Always grant projected focus -- this app has no native content
    // competing for the video surface.
    aap_protobuf::service::media::video::message::VideoFocusNotification indication;
    indication.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    indication.set_unsolicited(false);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: video focus indication send failed: %s\n", e.what());
        });
    channel_->sendVideoFocusIndication(indication, promise);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onChannelError(const aasdk::error::Error & e) {
    std::printf("androidauto: video channel error: %s\n", e.what());
}

}  // namespace androidauto
