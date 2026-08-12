#include "androidauto/session.h"

#include <cstdio>

#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>

namespace androidauto {

Session::Session(boost::asio::io_service &ioService)
    : ioService_(ioService), strand_(ioService) {
}

void Session::start(aasdk::transport::ITransport::Pointer transport) {
    auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
    cryptor_ = std::make_shared<aasdk::messenger::Cryptor>(std::move(sslWrapper));
    cryptor_->init();

    auto messageInStream = std::make_shared<aasdk::messenger::MessageInStream>(ioService_, transport, cryptor_);
    auto messageOutStream = std::make_shared<aasdk::messenger::MessageOutStream>(ioService_, transport, cryptor_);
    auto messenger = std::make_shared<aasdk::messenger::Messenger>(ioService_, messageInStream, messageOutStream);

    controlChannel_ = std::make_shared<aasdk::channel::control::ControlServiceChannel>(strand_, messenger);
    controlChannel_->receive(this->shared_from_this());

    inputChannel_ = std::make_shared<InputChannel>(strand_, messenger);

    // touchForwarder_ opens its own second evdev fd against the same
    // device node LVGL already reads (see touch_forwarder.h) -- deferred
    // until the phone actually opens the input channel, not started
    // eagerly here alongside construction. weak_ptr in the callback:
    // Session doesn't want to keep a TouchForwarder alive past its own
    // lifetime, and the callback is stored on inputChannel_, which
    // outlives this particular capture concern anyway, but weak_ptr
    // costs nothing and avoids a subtle lifetime assumption either way.
    touchForwarder_ = std::make_shared<TouchForwarder>(ioService_, inputChannel_);
    std::weak_ptr<TouchForwarder> weakTouchForwarder = touchForwarder_;
    inputChannel_->setChannelOpenCallback([weakTouchForwarder]() {
        if (auto forwarder = weakTouchForwarder.lock()) {
            if (!forwarder->start()) {
                std::printf("androidauto: touch forwarder failed to start -- "
                             "Android Auto session continues without touch input\n");
            }
        }
    });

    inputChannel_->start();

    // Video + the three audio sink channels, constructed and armed
    // alongside inputChannel_ -- see session.h's member comment and
    // Session::onServiceDiscoveryRequest() for the matching
    // advertisement. PCM device strings/rates are the real confirmed
    // routes from docs/AUDIO_SUBSYSTEM_INVESTIGATION.md (SYSTEM_AUDIO's
    // plug:softvol4 route is an explicitly-flagged approximation, not
    // an independently confirmed 1:1 mapping -- see that doc).
    videoChannel_ = std::make_shared<VideoChannel>(strand_, messenger);
    videoChannel_->start();

    audioChannelMedia_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO, "plug:softvol2", 48000, 2);
    audioChannelMedia_->start();

    audioChannelGuidance_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO, "plug:softvol1", 16000, 1);
    audioChannelGuidance_->start();

    audioChannelSystem_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO, "plug:softvol4", 16000, 1);
    audioChannelSystem_->start();

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("androidauto: version request sent\n"); },
        [](const aasdk::error::Error &e) {
            std::printf("androidauto: version request send failed: %s\n", e.what());
        });
    std::printf("androidauto: AOAP device ready, sending version request...\n");
    controlChannel_->sendVersionRequest(promise);
}

void Session::continueSSLHandshake() {
    bool active = false;
    try {
        active = cryptor_->doHandshake();
    } catch (const aasdk::error::Error &e) {
        std::printf("androidauto: SSL handshake failed: %s\n", e.what());
        return;
    }
    std::printf("androidauto: continueSSLHandshake: doHandshake() active=%d\n", active);

    auto outBuffer = cryptor_->readHandshakeBuffer();
    // 2026-08-12: logging every call unconditionally (even an empty
    // outBuffer) -- previously this branch was silent on the success
    // path, so a real hardware run where the connection died right
    // after "SSL handshake complete" (TCP EOF, every channel erroring
    // at once, ServiceDiscoveryRequest never received) left no way to
    // tell whether a final handshake flight was actually sent in
    // response to the phone's last payload, or whether outBuffer was
    // empty when the phone's own OpenSSL state machine may have still
    // been expecting one more message from us.
    std::printf("androidauto: continueSSLHandshake: outBuffer size=%zu\n", outBuffer.size());
    if (!outBuffer.empty()) {
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then(
            []() { std::printf("androidauto: handshake buffer sent\n"); },
            [](const aasdk::error::Error &e) {
                std::printf("androidauto: handshake send failed: %s\n", e.what());
            });
        controlChannel_->sendHandshake(std::move(outBuffer), promise);
    }

    if (active) {
        std::printf("androidauto: SSL handshake complete\n");
    }
}

void Session::onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                                 aap_protobuf::shared::MessageStatus status) {
    std::printf("androidauto: version response %u.%u, status=%d\n", majorCode, minorCode,
                static_cast<int>(status));
    this->continueSSLHandshake();
    controlChannel_->receive(this->shared_from_this());
}

void Session::onHandshake(const aasdk::common::DataConstBuffer &payload) {
    std::printf("androidauto: handshake payload received (%zu bytes)\n", payload.size);
    cryptor_->writeHandshakeBuffer(payload);
    this->continueSSLHandshake();
    controlChannel_->receive(this->shared_from_this());
}

void Session::onServiceDiscoveryRequest(
    const aap_protobuf::service::control::message::ServiceDiscoveryRequest &request) {
    std::printf("androidauto: service discovery request from '%s'\n", request.device_name().c_str());

    // Advertises InputSourceService (touch only, 800x480 matching this
    // device's real framebuffer -- see docs/ARCHITECTURE.md's Display
    // section), one MediaSinkService for video (VIDEO_800x480 H264_BP,
    // the exact real screen resolution -- no scaling needed) and one
    // each for the three audio types this app can actually play (see
    // Session::start()'s PCM route comment). Sensor services still
    // aren't implemented, so still not advertised -- advertising a
    // channel we can't actually open would be worse than not
    // advertising it.
    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    response.mutable_headunit_info()->set_head_unit_make("custom_ui");
    response.mutable_headunit_info()->set_head_unit_model("prado-firmware-reconstruction");
    response.set_display_name("custom_ui");
    response.set_driver_position(aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);

    auto *inputService = response.add_channels();
    // See input_channel.h's header comment for the service_id-numbering
    // caveat -- this uses aasdk's own ChannelId ordinal as a best-
    // available proxy, not an independently confirmed wire value.
    inputService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE));
    auto *touchscreen = inputService->mutable_input_source_service()->add_touchscreen();
    touchscreen->set_width(800);
    touchscreen->set_height(480);

    auto *videoService = response.add_channels();
    videoService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));
    auto *videoSink = videoService->mutable_media_sink_service();
    videoSink->set_available_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    auto *videoConfig = videoSink->add_video_configs();
    videoConfig->set_codec_resolution(
        aap_protobuf::service::media::sink::message::VIDEO_800x480);
    videoConfig->set_frame_rate(aap_protobuf::service::media::sink::message::VIDEO_FPS_30);
    videoConfig->set_video_codec_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);

    auto *mediaAudioService = response.add_channels();
    mediaAudioService->set_id(
        static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO));
    auto *mediaAudioSink = mediaAudioService->mutable_media_sink_service();
    mediaAudioSink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    mediaAudioSink->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA);
    auto *mediaAudioConfig = mediaAudioSink->add_audio_configs();
    mediaAudioConfig->set_sampling_rate(48000);
    mediaAudioConfig->set_number_of_bits(16);
    mediaAudioConfig->set_number_of_channels(2);

    auto *guidanceAudioService = response.add_channels();
    guidanceAudioService->set_id(
        static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO));
    auto *guidanceAudioSink = guidanceAudioService->mutable_media_sink_service();
    guidanceAudioSink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    guidanceAudioSink->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE);
    auto *guidanceAudioConfig = guidanceAudioSink->add_audio_configs();
    guidanceAudioConfig->set_sampling_rate(16000);
    guidanceAudioConfig->set_number_of_bits(16);
    guidanceAudioConfig->set_number_of_channels(1);

    auto *systemAudioService = response.add_channels();
    systemAudioService->set_id(
        static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO));
    auto *systemAudioSink = systemAudioService->mutable_media_sink_service();
    systemAudioSink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    systemAudioSink->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO);
    auto *systemAudioConfig = systemAudioSink->add_audio_configs();
    systemAudioConfig->set_sampling_rate(16000);
    systemAudioConfig->set_number_of_bits(16);
    systemAudioConfig->set_number_of_channels(1);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("androidauto: service discovery response sent\n"); },
        [](const aasdk::error::Error &e) {
            std::printf("androidauto: service discovery response send failed: %s\n", e.what());
        });
    controlChannel_->sendServiceDiscoveryResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onAudioFocusRequest(const aap_protobuf::service::control::message::AudioFocusRequest &) {
    std::printf("androidauto: audio focus request (not yet handled)\n");
    controlChannel_->receive(this->shared_from_this());
}

void Session::onByeByeRequest(const aap_protobuf::service::control::message::ByeByeRequest &) {
    std::printf("androidauto: bye-bye request\n");
    controlChannel_->receive(this->shared_from_this());
}

void Session::onByeByeResponse(const aap_protobuf::service::control::message::ByeByeResponse &) {
    std::printf("androidauto: bye-bye response\n");
    controlChannel_->receive(this->shared_from_this());
}

void Session::onBatteryStatusNotification(
    const aap_protobuf::service::control::message::BatteryStatusNotification &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onNavigationFocusRequest(
    const aap_protobuf::service::control::message::NavFocusRequestNotification &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onVoiceSessionRequest(
    const aap_protobuf::service::control::message::VoiceSessionNotification &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onPingRequest(const aap_protobuf::service::control::message::PingRequest &request) {
    // Confirmed protocol contract (IControlServiceChannel::
    // sendPingResponse), not speculative -- the phone uses ping/pong
    // as a keep-alive; echoing the timestamp back is the whole
    // contract, per PingResponse's own single required field.
    aap_protobuf::service::control::message::PingResponse response;
    response.set_timestamp(request.timestamp());

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error &e) {
            std::printf("androidauto: ping response send failed: %s\n", e.what());
        });
    controlChannel_->sendPingResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onPingResponse(const aap_protobuf::service::control::message::PingResponse &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onChannelError(const aasdk::error::Error &e) {
    std::printf("androidauto: control channel error: %s\n", e.what());
}

}  // namespace androidauto
