// The Android Auto MEDIA_SOURCE_MICROPHONE channel -- this project's
// ServiceDiscoveryResponse never advertised any MediaSourceService
// channel at all until now. Found missing 2026-08-12 by a fresh-eyes
// subagent review (chasing a real hardware bug: session gets through
// SSL handshake, ServiceDiscovery, ping/pong, and an early
// AudioFocusRequest cleanly, then the phone silently closes the TCP
// connection ~800ms later, never opening a single channel, never
// sending ByeByeRequest) -- github.com/mossyhub/openautolink's
// live_session.cpp advertises MEDIA_SOURCE_MICROPHONE as a first-class
// channel, and the real stock libAndroidAuto.so on this exact hardware
// has a full AudioSource endpoint class (see
// project_stock_libandroidauto_reference.md memory) -- this project's
// own aasdk fork already has the complete unimplemented
// IMediaSourceService/IMediaSourceServiceEventHandler infrastructure
// sitting unused, same shape sensor_channel.h's fix exploited for
// SENSOR.
//
// Deliberately minimal, same "advertise what you structurally
// support, stub the data path" reasoning as InputChannel (touch-only)
// and SensorChannel (DRIVING_STATUS_DATA-only): answers
// ChannelOpenRequest, MediaChannelSetupRequest, and the mic-specific
// open/close handshake (MicrophoneRequest/MicrophoneResponse)
// correctly, but doesn't actually capture or stream any real
// microphone audio yet -- there's no voice-input feature in this app
// to wire it to. The point is structural completeness (so a phone that
// gates connection viability on this channel existing doesn't have a
// reason to bail), not a working voice assistant.
#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceService.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>

namespace androidauto {

class MicrophoneChannel : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler,
                           public std::enable_shared_from_this<MicrophoneChannel> {
public:
    using Pointer = std::shared_ptr<MicrophoneChannel>;

    MicrophoneChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger);

    // Arms the channel's receive loop -- call once, right after
    // construction (deferred until after ServiceDiscoveryResponse is
    // confirmed sent, same as every other channel -- see session.cpp).
    void start();

    void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
    void onMediaChannelSetupRequest(const aap_protobuf::service::media::shared::message::Setup &request) override;
    void onMediaSourceOpenRequest(
        const aap_protobuf::service::media::source::message::MicrophoneRequest &request) override;
    void onMediaChannelAckIndication(
        const aap_protobuf::service::media::source::message::Ack &indication) override;
    void onChannelError(const aasdk::error::Error &e) override;

private:
    boost::asio::io_service::strand &strand_;
    aasdk::channel::mediasource::IMediaSourceService::Pointer channel_;
    std::int32_t sessionId_ = 0;
};

}  // namespace androidauto
