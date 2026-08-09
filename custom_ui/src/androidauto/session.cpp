#include "androidauto/session.h"

#include <cstdio>

#include <aasdk/Transport/SSLWrapper.hpp>
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
    // Not yet answered with a real service discovery response (that
    // requires actually describing the services we support -- media
    // sink, input, sensor, etc.) -- see docs/IMPLEMENTATION_PLAN.md.
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

void Session::onPingRequest(const aap_protobuf::service::control::message::PingRequest &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onPingResponse(const aap_protobuf::service::control::message::PingResponse &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onChannelError(const aasdk::error::Error &e) {
    std::printf("androidauto: control channel error: %s\n", e.what());
}

}  // namespace androidauto
