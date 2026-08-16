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

    // 2026-08-16: found on real hardware -- once video was actually
    // reaching the right layer with the right colors, real footage
    // still showed tearing-like artifacts. Root cause, found by
    // reviewing this project's own kernel source
    // (linux-arkmicro/.../ark1668_lcdc_funcs.c, ARKFB_SET_VIDEO_ADDR_RAW's
    // own comment): this device's driver applies each address update
    // synchronously and immediately, with no real double-buffering or
    // vsync gating (a deliberate simplification vs stock's real
    // IRQ-driven queue, documented in that file's own comment) --
    // pushing a new address while the panel is mid-scanout of the
    // previous one tears. Rather than touch the kernel driver (per
    // explicit request -- stock's own real double-buffered pipeline
    // proves this same hardware/config CAN work correctly, so the fix
    // belongs in how often userspace pushes, not in reimplementing
    // stock's private kernel-side machinery), this throttles from
    // here: skip the address update (not the frame -- decode/ack still
    // proceed normally) if less than ~16ms (one panel refresh at a
    // plausible 60Hz) has passed since the last one. A well-behaved
    // ~30fps H.264 stream (a new picture roughly every 33ms) rarely
    // even hits this; it mainly caps bursts (several pictures becoming
    // ready in quick succession after a stall) from slamming multiple
    // address updates within the same refresh window.
    constexpr std::chrono::milliseconds kMinAddrPushInterval{16};
    auto now = std::chrono::steady_clock::now();
    if (lastAddrPush_.time_since_epoch().count() != 0 && (now - lastAddrPush_) < kMinAddrPushInterval) {
        return;
    }

    if (!hal::set_frame_addr(videoLayer_, pic.outputPictureBusAddress, pic.picWidth,
                              pic.picHeight)) {
        return;
    }
    lastAddrPush_ = now;

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
