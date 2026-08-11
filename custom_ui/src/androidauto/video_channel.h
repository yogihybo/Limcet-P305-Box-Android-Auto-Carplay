// The Android Auto video sink channel. Wraps aasdk::channel::mediasink::
// video::VideoMediaSinkService, implements
// IVideoMediaSinkServiceEventHandler, decodes received H.264 via
// HantroH264Decoder (see that class's header comment for the real
// kernel-driver-confirmed reason libmfc.so is necessary, and the real,
// scoped gap in actually displaying a decoded frame).
//
// Advertises exactly one VideoConfiguration: VIDEO_800x480 (a real
// enum value in aasdk's own vendored schema matching this device's
// exact screen resolution, no scaling needed) + MEDIA_CODEC_VIDEO_H264_BP,
// same "one fixed config, no real negotiation" reasoning as
// AudioChannel.
//
// NOT YET hardware-tested. Even once a phone connects and streams
// real H.264 data, decoded frames can't reach the screen yet -- see
// hantro_h264_decoder.h's header comment for the specific, scoped gap
// (H264DecPicture's plane-address layout isn't reverse-engineered).
// This class still does everything up to that point for real: channel
// open, setup negotiation, focus grant, and feeding real received data
// into the real decoder.
#pragma once

#include <cstdint>
#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Messenger/IMessenger.hpp>

#include "androidauto/hantro_h264_decoder.h"

namespace androidauto {

class VideoChannel : public aasdk::channel::mediasink::video::IVideoMediaSinkServiceEventHandler,
                      public std::enable_shared_from_this<VideoChannel> {
public:
    using Pointer = std::shared_ptr<VideoChannel>;

    VideoChannel(boost::asio::io_service::strand & strand, aasdk::messenger::IMessenger::Pointer messenger);

    void start();

    void onChannelOpenRequest(
        const aap_protobuf::service::control::message::ChannelOpenRequest & request) override;
    void onMediaChannelSetupRequest(
        const aap_protobuf::service::media::shared::message::Setup & request) override;
    void onMediaChannelStartIndication(
        const aap_protobuf::service::media::shared::message::Start & indication) override;
    void onMediaChannelStopIndication(
        const aap_protobuf::service::media::shared::message::Stop & indication) override;
    void onMediaWithTimestampIndication(aasdk::messenger::Timestamp::ValueType timestamp,
                                         const aasdk::common::DataConstBuffer & buffer) override;
    void onMediaIndication(const aasdk::common::DataConstBuffer & buffer) override;
    void onVideoFocusRequest(
        const aap_protobuf::service::media::video::message::VideoFocusRequestNotification & request)
        override;
    void onChannelError(const aasdk::error::Error & e) override;

private:
    void decodeBuffer(const aasdk::common::DataConstBuffer & buffer);
    void sendAck();

    boost::asio::io_service::strand & strand_;
    aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel_;

    HantroH264Decoder decoder_;
    bool decoderOpen_ = false;

    int32_t sessionId_ = 0;
    uint64_t ackCount_ = 0;
};

}  // namespace androidauto
