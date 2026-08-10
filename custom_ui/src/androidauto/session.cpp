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

    auto outBuffer = cryptor_->readHandshakeBuffer();
    if (!outBuffer.empty()) {
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then(
            []() {},
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
    // section) -- video/audio/sensor services still aren't implemented,
    // so still not advertised (advertising a channel we can't actually
    // open would be worse than not advertising it).
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
