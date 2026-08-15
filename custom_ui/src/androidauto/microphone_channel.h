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
// 2026-08-15: real microphone capture wired in, replacing the old
// structural-only stub -- see project_mic_capture_investigation memory
// for the hardware-confirmed prerequisites this builds on directly
// rather than re-deriving: the kernel-side SARADC settle-delay fix
// (linux-arkmicro 4a1d8a213) and the confirmation that MsnCoreApp's own
// `sink` binary successfully captures real audio via ALSA at "hw:0,0"
// once that fix is in place. This class opens the exact same
// underlying ALSA capture path (via AlsaInput, see its own header
// comment) rather than anything MsnCoreApp/libMsnSound.so-specific
// (that whole layer -- MsnProductInfo.ini's SoundType, /data/msncfg/
// userdata caching -- only gates MsnCoreApp's OWN init path, not raw
// ALSA, so none of it is relevant to this process).
//
// Same "advertise what you structurally support" shape as InputChannel/
// SensorChannel otherwise: answers ChannelOpenRequest and
// MediaChannelSetupRequest the same as before; the mic-specific open/
// close handshake (MicrophoneRequest/MicrophoneResponse) now ALSO
// starts/stops a dedicated capture thread (blocking snd_pcm_readi()
// can't run on the shared strand thread) that forwards real captured
// PCM frames upstream via sendMediaSourceWithTimestampIndication(),
// posted through strand_ same as every other cross-thread channel
// operation in this codebase (see Session::sendInputKey's own
// comment).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include <boost/asio.hpp>

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceService.hpp>
#include <aasdk/Channel/MediaSource/IMediaSourceServiceEventHandler.hpp>

#include "androidauto/alsa_input.h"

namespace androidauto {

class MicrophoneChannel : public aasdk::channel::mediasource::IMediaSourceServiceEventHandler,
                           public std::enable_shared_from_this<MicrophoneChannel> {
public:
    using Pointer = std::shared_ptr<MicrophoneChannel>;

    MicrophoneChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger);
    ~MicrophoneChannel();

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
    // Runs on its own dedicated thread (captureThread_) -- loops
    // blocking snd_pcm_readi() calls via alsaInput_ and posts each
    // frame's worth of PCM data through strand_ to actually send it.
    // Exits when capturing_ is cleared.
    void captureLoop();

    // Joins captureThread_ (if running) and closes alsaInput_ -- shared
    // by the close-request path and the destructor, same "one place,
    // called from both a normal-shutdown path and a defensive
    // destructor path" convention as android_auto_screen.cpp's
    // screen_delete_cb().
    void stopCapture();

    boost::asio::io_service::strand &strand_;
    aasdk::channel::mediasource::IMediaSourceService::Pointer channel_;
    std::int32_t sessionId_ = 0;

    std::unique_ptr<AlsaInput> alsaInput_;
    std::thread captureThread_;
    std::atomic<bool> capturing_{false};
};

}  // namespace androidauto
