#include "androidauto/video_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"
#include "androidauto/video_visibility.h"

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
    std::printf("%s androidauto: video channel open request (priority=%d)\n", logTimestamp().c_str(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: video channel open response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error & e) {
            std::printf("%s androidauto: video channel open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup & request) {
    std::printf("%s androidauto: video channel setup request, codec type=%d\n", logTimestamp().c_str(),
               static_cast<int>(request.type()));

    // Only one configuration is ever advertised (VIDEO_800x480 H264_BP,
    // see Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    // 2026-08-15: same fix as audio_channel.cpp's own comment -- real
    // phone-side adb logcat caught "MaxUnacked must be >= 0, was 0"
    // right before teardown. 1 matches microphone_channel.cpp's
    // already-correct value / the real reference.
    response.set_max_unacked(1);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this, self = shared_from_this()]() {
            std::printf("%s androidauto: video channel setup response sent\n", logTimestamp().c_str());
            // 2026-08-15: found via the real opencardev/openauto
            // reference (VideoService::onAVChannelSetupRequest()) --
            // the head unit is expected to proactively grant video
            // focus right after setup, not just wait to react if/when
            // the phone happens to ask. Real hardware showed exactly
            // the symptom this predicts: clean channel open/setup for
            // every channel, ping keep-alive running fine both
            // directions, but the phone never sent a
            // VideoFocusRequest or MediaChannelStartIndication at all
            // -- it was waiting for a grant this code never sent.
            sendVideoFocusIndication(/*unsolicited=*/true);
        },
        [](const aasdk::error::Error & e) {
            std::printf("%s androidauto: video channel setup response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start & indication) {
    sessionId_ = indication.session_id();
    std::printf("%s androidauto: video channel start, session_id=%d config_index=%u\n", logTimestamp().c_str(),
               sessionId_, indication.configuration_index());

    if (!decoderOpen_) {
        decoderOpen_ = decoder_.open();
        if (!decoderOpen_) {
            std::printf("%s androidauto: video decoder open failed -- no video for this session\n",
                        logTimestamp().c_str());
        }
    }

    channel_->receive(this->shared_from_this());
}

void VideoChannel::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop &) {
    std::printf("%s androidauto: video channel stop\n", logTimestamp().c_str());
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
        if (decoder_.decodeFrame(buffer.cdata, buffer.size)) {
            pushDecodedFrame();
        }
    }
    sendAck();
}

void VideoChannel::pushDecodedFrame() {
    const H264DecPicture & pic = decoder_.last_picture();

    // outputFormat 1 = 8x4 tiled -- hal::set_frame_addr()'s chroma
    // offset math (width*height bytes past the Y plane start) is only
    // valid for raster-scan (outputFormat 0). Tiled layout uses a
    // completely different addressing scheme this code doesn't
    // implement -- bail with a clear log rather than push a garbled
    // frame. Which format the decoder actually picks isn't controlled
    // by this class (H264DecInit's args, see hantro_h264_decoder.h);
    // not yet observed on real hardware which one this device uses.
    if (pic.outputFormat != 0) {
        std::printf("androidauto: video: decoded picture is tiled (outputFormat=%u), "
                   "not raster scan -- display push not implemented for this layout, "
                   "skipping frame\n", pic.outputFormat);
        return;
    }

    if (!videoLayerOpen_) {
        videoLayerOpen_ = hal::init_video_layer(videoLayer_);
        if (!videoLayerOpen_) {
            std::printf("androidauto: video layer unavailable -- decoded frames have nowhere "
                       "to go\n");
            return;
        }
    }

    if (!videoLayerConfigured_ || pic.picWidth != configuredWidth_ ||
        pic.picHeight != configuredHeight_) {
        if (!hal::configure_video_layer(videoLayer_, pic.picWidth, pic.picHeight)) {
            return;
        }
        configuredWidth_ = pic.picWidth;
        configuredHeight_ = pic.picHeight;
        videoLayerConfigured_ = true;
    }

    // 2026-08-16: three earlier fix attempts chased a blocky-
    // corruption artifact by adding per-frame vsync handling here --
    // a fixed ~16ms software throttle, then a blocking
    // hal::wait_for_vsync(), then a hal::get_frame_addr()-confirm
    // loop modeled on real decompiled code. All three removed:
    // decompiling the ACTUAL real stock Android Auto video path
    // (usr/bin/sink's own VideoDecoder::flush_video(), not a guess or
    // a different app) shows it does none of this -- it calls
    // arkapi_set_fb_addr() unconditionally, every single frame, with
    // no wait and no confirm-loop around it. hal::set_frame_addr()
    // now matches that exactly: called directly, every ready frame,
    // no gating.
    //
    // 2026-08-17: the corruption persisted even after matching stock's
    // display API and push cadence exactly -- root cause turned out to
    // be upstream of the display push entirely. Real hardware dmesg
    // showed the Hantro decoder itself only ever allocates TWO
    // internal reference-picture buffers, no third for safety margin,
    // so pushing pic.outputPictureBusAddress directly raced the
    // decoder overwriting that same physical buffer for the next frame
    // while the LCDC was still scanning it out for this one -- see
    // HantroH264Decoder::stabilize_output()'s own doc comment for the
    // full story. Copy into a buffer this side owns before pushing,
    // rather than pointing the display at the decoder's own memory.
    uint32_t stableAddr = decoder_.stabilize_output();
    if (stableAddr == 0) {
        return;
    }
    if (!hal::set_frame_addr(videoLayer_, stableAddr, pic.picWidth, pic.picHeight)) {
        return;
    }

    // 2026-08-12: reconciles the hardware layer's actual shown/hidden
    // state against video_visible() on every frame, instead of a
    // one-shot "show after the first real frame" -- see
    // video_visibility.h's own comment for why: with auto-start,
    // decode can be running well before the user has selected the AA
    // icon, and this layer previously had no way to stay hidden until
    // they do. Still gated on the first real frame address having been
    // set (showing an unconfigured/zero-address window would display
    // garbage for one frame's worth of time) -- video_visible() alone
    // can't have gone true before that anyway, since nothing calls
    // setVisible(true) before the AA screen -- which only opens once a
    // session exists -- is on screen, but the ordering guard costs
    // nothing to keep explicit.
    bool wantVisible = video_visible().load(std::memory_order_acquire);
    if (wantVisible && !videoLayerShown_) {
        hal::show_video_layer(videoLayer_);
        videoLayerShown_ = true;
    } else if (!wantVisible && videoLayerShown_) {
        hal::hide_video_layer(videoLayer_);
        videoLayerShown_ = false;
    }
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
    std::printf("%s androidauto: video focus request, mode=%d reason=%d\n", logTimestamp().c_str(),
               static_cast<int>(request.mode()), static_cast<int>(request.reason()));

    // Always grant projected focus -- this app has no native content
    // competing for the video surface.
    sendVideoFocusIndication(/*unsolicited=*/false);

    channel_->receive(this->shared_from_this());
}

void VideoChannel::sendVideoFocusIndication(bool unsolicited) {
    aap_protobuf::service::media::video::message::VideoFocusNotification indication;
    indication.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    indication.set_unsolicited(unsolicited);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [unsolicited]() {
            std::printf("%s androidauto: video focus indication sent (unsolicited=%d)\n",
                       logTimestamp().c_str(), unsolicited);
        },
        [](const aasdk::error::Error & e) {
            std::printf("androidauto: video focus indication send failed: %s\n", e.what());
        });
    channel_->sendVideoFocusIndication(indication, promise);
}

void VideoChannel::onChannelError(const aasdk::error::Error & e) {
    std::printf("%s androidauto: video channel error: %s\n", logTimestamp().c_str(), e.what());
}

}  // namespace androidauto
