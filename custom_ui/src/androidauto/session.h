#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/USB/IUSBWrapper.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Channel/Control/IControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>

namespace androidauto {

// Drives the real Android Auto control-channel handshake against a
// device aasdk::usb::USBHub has already negotiated into AOAP
// accessory mode: sends the version request, then drives aasdk's
// OpenSSL-BIO-based Cryptor through the handshake loop (the head unit
// is the TLS *client* here -- Cryptor::init() calls
// SSL_set_connect_state internally), then logs the service discovery
// request once the phone sends it.
//
// Scoped deliberately: does not yet send a service discovery
// RESPONSE or open any actual media/input/sensor channel -- that's
// the next increment (see docs/IMPLEMENTATION_PLAN.md Phase 2). The
// receive()-then-re-arm pattern used throughout (call
// controlChannel_->receive(shared_from_this()) again at the end of
// every on*() handler) mirrors aasdk's own internal
// src/Channel/Bluetooth/BluetoothService.cpp, the reference used to
// confirm ControlServiceChannel does NOT auto-rearm itself after
// dispatching a known message -- only its own "unhandled message id"
// fallback path does.
//
// Not hardware-tested against a real phone -- built directly against
// aasdk's documented class contracts, but the actual byte-level SSL
// exchange has not been observed against a live device yet.
class Session : public aasdk::channel::control::IControlServiceChannelEventHandler,
                public std::enable_shared_from_this<Session> {
public:
    using Pointer = std::shared_ptr<Session>;

    Session(boost::asio::io_service &ioService, aasdk::usb::IUSBWrapper &usbWrapper);

    // deviceHandle must already be AOAP-negotiated (i.e. resolved by
    // aasdk::usb::USBHub's accessory-mode query chain, as in
    // androidauto::run_usb_probe). Wraps it in a USBTransport +
    // Messenger + ControlServiceChannel and sends the version request.
    void start(aasdk::usb::DeviceHandle deviceHandle);

    void onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                            aap_protobuf::shared::MessageStatus status) override;
    void onHandshake(const aasdk::common::DataConstBuffer &payload) override;
    void onServiceDiscoveryRequest(
        const aap_protobuf::service::control::message::ServiceDiscoveryRequest &request) override;
    void onAudioFocusRequest(
        const aap_protobuf::service::control::message::AudioFocusRequest &request) override;
    void onByeByeRequest(const aap_protobuf::service::control::message::ByeByeRequest &request) override;
    void onByeByeResponse(
        const aap_protobuf::service::control::message::ByeByeResponse &response) override;
    void onBatteryStatusNotification(
        const aap_protobuf::service::control::message::BatteryStatusNotification &notification) override;
    void onNavigationFocusRequest(
        const aap_protobuf::service::control::message::NavFocusRequestNotification &request) override;
    void onVoiceSessionRequest(
        const aap_protobuf::service::control::message::VoiceSessionNotification &request) override;
    void onPingRequest(const aap_protobuf::service::control::message::PingRequest &request) override;
    void onPingResponse(const aap_protobuf::service::control::message::PingResponse &response) override;
    void onChannelError(const aasdk::error::Error &e) override;

private:
    // Advances cryptor_'s SSL BIO state machine one step and, if it
    // produced outbound handshake bytes, sends them over the control
    // channel. Called once to kick off the handshake (from
    // onVersionResponse) and again every time the phone's own
    // handshake bytes arrive (onHandshake), until cryptor_->isActive().
    void continueSSLHandshake();

    boost::asio::io_service &ioService_;
    aasdk::usb::IUSBWrapper &usbWrapper_;
    boost::asio::io_service::strand strand_;
    aasdk::messenger::ICryptor::Pointer cryptor_;
    aasdk::channel::control::IControlServiceChannel::Pointer controlChannel_;
};

}  // namespace androidauto
