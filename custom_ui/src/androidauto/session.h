#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Channel/Control/IControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>

#include "androidauto/input_channel.h"

namespace androidauto {

// Drives the real Android Auto control-channel handshake over an
// already-established transport: sends the version request, then
// drives aasdk's OpenSSL-BIO-based Cryptor through the handshake loop
// (the head unit is the TLS *client* here -- Cryptor::init() calls
// SSL_set_connect_state internally), then logs the service discovery
// request once the phone sends it.
//
// Deliberately transport-agnostic (takes an aasdk::transport::ITransport
// already wrapping either a USB AOAP device or a TCP socket) -- the
// control-channel/Messenger/Cryptor logic is identical either way, only
// how bytes get to the phone differs. See usb_probe.cpp for the wired
// (USBTransport) caller; the wireless (TCPTransport) caller lands
// alongside the Bluetooth/WifiProjection pairing flow -- see
// docs/IMPLEMENTATION_PLAN.md Phase 2's "Wireless AA" section for why
// that's a required path here, not a nice-to-have (the device's one
// external USB port is normally occupied by the boot rootfs drive).
//
// Scoped deliberately: does not yet send a service discovery
// RESPONSE or open any actual media/input/sensor channel -- that's
// the next increment. The receive()-then-re-arm pattern used
// throughout (call controlChannel_->receive(shared_from_this()) again
// at the end of every on*() handler) mirrors aasdk's own internal
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

    Session(boost::asio::io_service &ioService);

    // transport must already be established and ready to exchange
    // bytes (a USBTransport wrapping an AOAP-negotiated device, or a
    // TCPTransport wrapping a connected socket). Wraps it in a
    // Messenger + ControlServiceChannel and sends the version request.
    void start(aasdk::transport::ITransport::Pointer transport);

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
    boost::asio::io_service::strand strand_;
    aasdk::messenger::ICryptor::Pointer cryptor_;
    aasdk::channel::control::IControlServiceChannel::Pointer controlChannel_;
    // Constructed and armed (start()'d) alongside the control channel
    // in Session::start(), on the same Messenger -- so it's ready to
    // catch the phone's ChannelOpenRequest whenever it decides to send
    // one, independent of when/whether ServiceDiscoveryResponse
    // actually gets answered.
    InputChannel::Pointer inputChannel_;
};

}  // namespace androidauto
