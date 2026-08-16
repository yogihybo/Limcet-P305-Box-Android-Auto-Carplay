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
// Decoded frames now get pushed to the real hardware video-overlay
// layer (hal/video_layer.h -- /dev/fb4/VIDEO_LAYER2, see that file's
// own top comment for the real, kernel-source-confirmed fb-to-layer
// mapping) via pushDecodedFrame(), called from decodeBuffer() once
// HantroH264Decoder reports a picture ready. See video_layer.h's own
// top comment for exactly what's ground-truth-confirmed (the ioctl
// numbers/struct/device node) vs. still an assumption (the semi-planar
// chroma-offset math, not yet checked against a real decoded frame).
//
// NOT YET hardware-tested end to end -- channel open, setup
// negotiation, focus grant, feeding real received data into the real
// decoder, and now the display push are all implemented for real, but
// nothing here has been exercised against an actual phone connection
// and a real decoded frame yet.
#pragma once

#include <cstdint>
#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Video/VideoMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Video/IVideoMediaSinkServiceEventHandler.hpp>
#include <aasdk/Messenger/IMessenger.hpp>

#include "androidauto/hantro_h264_decoder.h"
#include "hal/video_layer.h"

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
    // Grants VIDEO_FOCUS_PROJECTED -- called both proactively, right
    // after channel setup completes (unsolicited=true, matching the
    // real opencardev/openauto reference's own onAVChannelSetupRequest
    // -- see git history for why this project's own version was
    // missing it), and reactively in response to an actual
    // VideoFocusRequestNotification (unsolicited=false).
    void sendVideoFocusIndication(bool unsolicited);
    // Pushes decoder_.last_picture() to hal::video_layer -- lazily
    // opens/configures the layer on the first ready picture (width/
    // height aren't known before then), reconfigures if the picture's
    // own dimensions ever change mid-session, then just updates the
    // frame address on every subsequent call.
    void pushDecodedFrame();

    boost::asio::io_service::strand & strand_;
    aasdk::channel::mediasink::video::IVideoMediaSinkService::Pointer channel_;

    HantroH264Decoder decoder_;
    bool decoderOpen_ = false;

    hal::VideoLayerHandle videoLayer_;
    bool videoLayerOpen_ = false;
    bool videoLayerConfigured_ = false;
    bool videoLayerShown_ = false;
    uint32_t configuredWidth_ = 0;
    uint32_t configuredHeight_ = 0;

    int32_t sessionId_ = 0;
    uint64_t ackCount_ = 0;
};

}  // namespace androidauto
