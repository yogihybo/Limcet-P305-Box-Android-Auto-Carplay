// One Android Auto audio sink channel (media / guidance / system --
// same class, different aasdk::messenger::ChannelId and PCM route, see
// alsa_output.h for the confirmed real PCM device strings per type).
// Wraps aasdk::channel::mediasink::audio::AudioMediaSinkService,
// implements IAudioMediaSinkServiceEventHandler, plays received PCM
// via AlsaOutput.
//
// Config is fixed, not negotiated per-message: this class advertises
// exactly ONE AudioConfiguration (see Session::onServiceDiscoveryRequest)
// and always answers Setup with configuration_index 0 selecting it --
// there's no real reason to advertise multiple configs when this app
// controls both ends of what it can actually play. AlsaOutput is
// opened lazily on the first Start indication (not eagerly at
// construction) so a channel that's advertised but never used doesn't
// hold an ALSA device open.
//
// NOT YET hardware-tested.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <aasdk/Channel/MediaSink/Audio/AudioMediaSinkService.hpp>
#include <aasdk/Channel/MediaSink/Audio/IAudioMediaSinkServiceEventHandler.hpp>
#include <aasdk/Messenger/IMessenger.hpp>

#include "androidauto/alsa_output.h"

namespace androidauto {

class AudioChannel : public aasdk::channel::mediasink::audio::IAudioMediaSinkServiceEventHandler,
                      public std::enable_shared_from_this<AudioChannel> {
public:
    using Pointer = std::shared_ptr<AudioChannel>;

    // pcmDevice: a real confirmed device string, see alsa_output.h.
    AudioChannel(boost::asio::io_service::strand & strand, aasdk::messenger::IMessenger::Pointer messenger,
                 aasdk::messenger::ChannelId channelId, std::string pcmDevice, uint32_t sampleRate,
                 uint32_t channels);

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
    void onChannelError(const aasdk::error::Error & e) override;

private:
    void playBuffer(const aasdk::common::DataConstBuffer & buffer);
    void sendAck();

    boost::asio::io_service::strand & strand_;
    aasdk::channel::mediasink::audio::IAudioMediaSinkService::Pointer channel_;

    std::string pcmDevice_;
    uint32_t sampleRate_;
    uint32_t channels_;
    AlsaOutput alsaOutput_;
    bool alsaOpen_ = false;

    // 2026-08-19: see alsa_output.h's own class comment -- read from
    // AlsaOutput's writer thread (to decide whether to post to
    // strand_ at all) and written/cleared from strand_ (playBuffer()/
    // the consumed callback), so this needs to be atomic even though
    // it's a simple counter.
    std::atomic<size_t> pendingPacedAcks_{0};

    int32_t sessionId_ = 0;
};

}  // namespace androidauto
