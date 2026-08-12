#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Channel/Control/IControlServiceChannel.hpp>
#include <aasdk/Channel/Control/IControlServiceChannelEventHandler.hpp>

#include "androidauto/audio_channel.h"
#include "androidauto/input_channel.h"
#include "androidauto/microphone_channel.h"
#include "androidauto/sensor_channel.h"
#include "androidauto/touch_forwarder.h"
#include "androidauto/video_channel.h"

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

    // 2026-08-12: the previously-deferred Pinger, now implemented --
    // real hardware got all the way through ServiceDiscoveryResponse
    // and then had every channel die together a few seconds later
    // (AASDK_ERROR TCP eof), the signature of the PHONE dropping the
    // connection, not a per-channel rejection. onServiceDiscoveryRequest()
    // advertises ping_configuration (interval_ms=1000,
    // tracked_ping_count=5) as a contract for how often the HEAD UNIT
    // will ping -- with no Pinger, that contract was never honored, so
    // the phone concluded the head unit was unresponsive and closed
    // the socket. Started once ServiceDiscoveryResponse is sent (there
    // is nothing meaningful to ping before the session is actually
    // established), stopped on a clean ByeBye or a real channel error
    // so it doesn't keep firing sendPingRequest() into a dead channel.
    void schedulePing();
    // Sends one PingRequest{timestamp=now_ms} immediately -- called
    // both by onServiceDiscoveryRequest() (the first ping, sent right
    // away rather than waiting a full interval_ms, matching
    // openautolink's live_session.cpp) and by schedulePing()'s own
    // timer callback (every subsequent one).
    void sendPing();

    boost::asio::io_service &ioService_;
    boost::asio::io_service::strand strand_;
    boost::asio::steady_timer pingTimer_;
    bool stopping_ = false;
    aasdk::messenger::ICryptor::Pointer cryptor_;
    aasdk::channel::control::IControlServiceChannel::Pointer controlChannel_;
    // Constructed in Session::start(), but NOT armed (->start(), i.e.
    // channel_->receive()) until Session::onServiceDiscoveryRequest()'s
    // send-success callback -- see that function's own comment. Matches
    // github.com/mossyhub/openautolink's live_session.cpp, whose
    // equivalent comment reads "NOW start all service handlers -- after
    // TLS + auth + discovery are complete". Previously armed
    // immediately in Session::start(), before the phone had even sent
    // VersionRequest -- a real, concrete divergence from the working
    // reference, found while chasing a "Communication error 2 -
    // incompatible software" screen on real hardware.
    InputChannel::Pointer inputChannel_;
    // Constructed alongside inputChannel_ in Session::start(), but its
    // own start() (opening the second evdev fd) is deferred until
    // inputChannel_'s channel-open callback fires -- see
    // touch_forwarder.h and input_channel.h's setChannelOpenCallback()
    // for why: no reason to hold a second touch fd open before the
    // phone has actually opened the channel to receive events on.
    TouchForwarder::Pointer touchForwarder_;

    // Constructed in Session::start(), armed the same deferred way as
    // inputChannel_ above (see its comment). videoChannel_ decodes via
    // HantroH264Decoder (real hardware decode works; actually
    // displaying a frame doesn't yet, see that class's header comment
    // for the specific gap). audioChannel*_ play via AlsaOutput against
    // the real, confirmed PCM device routes for each audio type -- see
    // alsa_output.h.
    VideoChannel::Pointer videoChannel_;
    AudioChannel::Pointer audioChannelMedia_;
    AudioChannel::Pointer audioChannelGuidance_;
    AudioChannel::Pointer audioChannelSystem_;
    // Constructed and armed the same deferred way as the channels
    // above -- see sensor_channel.h's header comment for why this
    // exists at all (real phones are widely known to require at least
    // DRIVING_STATUS_DATA to stay connected) and why it only ever
    // advertises/answers that one sensor.
    SensorChannel::Pointer sensorChannel_;
    // Constructed and armed the same deferred way as the channels
    // above -- see microphone_channel.h's header comment for why this
    // exists (this project never advertised any MediaSourceService
    // channel at all until a fresh-eyes review found the gap) and why
    // it's structural-only (no real mic capture wired in yet).
    MicrophoneChannel::Pointer microphoneChannel_;
};

}  // namespace androidauto
