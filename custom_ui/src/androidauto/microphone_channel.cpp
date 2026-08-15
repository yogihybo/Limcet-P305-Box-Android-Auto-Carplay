#include "androidauto/microphone_channel.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include "androidauto/log_timing.h"

#include <aasdk/Channel/MediaSource/MediaSourceService.hpp>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>

namespace androidauto {

namespace {

// Real device string/format, see alsa_input.h's own comment for full
// provenance. 16000/16/1 matches session.cpp's ServiceDiscoveryResponse
// exactly -- must, since that's the format the phone was told to
// expect.
constexpr const char * kMicDevice = "plug:hw:0,0";
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kBitsPerSample = 16;
constexpr uint32_t kChannels = 1;
// 10ms per chunk -- small enough to keep voice-input latency low
// (matters more here than for media playback), large enough not to
// make the capture thread spin needlessly. 16000 Hz / 100 = 160 frames.
constexpr uint32_t kFramesPerChunk = kSampleRate / 100;

std::uint64_t nowMicros() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

}  // namespace

MicrophoneChannel::MicrophoneChannel(boost::asio::io_service::strand &strand,
                                      aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
          strand, std::move(messenger), aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE)) {
}

MicrophoneChannel::~MicrophoneChannel() {
    // Defensive -- covers session teardown without a clean close
    // request (e.g. the phone just disconnects). Mirrors android_auto_
    // screen.cpp's screen_delete_cb() "restore on either the normal
    // path or destruction" pattern.
    stopCapture();
}

void MicrophoneChannel::start() {
    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("%s androidauto: microphone channel open request (priority=%d)\n", logTimestamp().c_str(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: microphone channel open response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: microphone channel open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup &request) {
    std::printf("%s androidauto: microphone channel setup request, codec type=%d\n", logTimestamp().c_str(),
                static_cast<int>(request.type()));

    // Only one configuration is ever advertised for this channel (see
    // Session::onServiceDiscoveryRequest) -- always select index 0.
    aap_protobuf::service::media::shared::message::Config response;
    response.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    // max_unacked=1: matches the real upstream f1x/openauto reference's
    // MediaSourceService::onMediaChannelSetupRequest() exactly (see
    // github.com/vteckz/MicStream's vendored copy) -- optional field,
    // not confirmed required, but no reason to diverge from a real
    // working implementation for this exact channel type.
    response.set_max_unacked(1);
    response.add_configuration_indices(0);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: microphone channel setup response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: microphone channel setup response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelSetupResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onMediaSourceOpenRequest(
    const aap_protobuf::service::media::source::message::MicrophoneRequest &request) {
    std::printf("%s androidauto: microphone %s request\n", logTimestamp().c_str(),
                request.open() ? "open" : "close");

    aap_protobuf::service::media::source::message::MicrophoneResponse response;
    response.set_status(static_cast<std::int32_t>(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS));
    response.set_session_id(sessionId_);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: microphone open response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: microphone open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendMicrophoneOpenResponse(response, promise);

    if (request.open()) {
        stopCapture();  // defensive -- in case a stray second open arrives without a close in between
        alsaInput_ = std::make_unique<AlsaInput>(kMicDevice, kSampleRate, kBitsPerSample, kChannels);
        if (alsaInput_->open()) {
            capturing_.store(true, std::memory_order_release);
            captureThread_ = std::thread(&MicrophoneChannel::captureLoop, this);
        } else {
            std::fprintf(stderr, "%s androidauto: microphone capture unavailable -- phone will get "
                         "silence\n", logTimestamp().c_str());
            alsaInput_.reset();
        }
    } else {
        stopCapture();
    }

    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::captureLoop() {
    std::vector<std::int16_t> samples(kFramesPerChunk * kChannels);

    while (capturing_.load(std::memory_order_acquire)) {
        int got = alsaInput_->read(samples.data(), kFramesPerChunk);
        if (got <= 0) {
            // AlsaInput::read() already logged the specific reason on a
            // real error; a genuine unrecoverable failure shouldn't
            // spin -- stop this capture session the same as a close
            // request would.
            break;
        }

        auto data = std::make_shared<aasdk::common::Data>(
            reinterpret_cast<const std::uint8_t *>(samples.data()),
            reinterpret_cast<const std::uint8_t *>(samples.data()) +
                static_cast<std::size_t>(got) * kChannels * (kBitsPerSample / 8));
        std::uint64_t timestamp = nowMicros();

        // Posted through strand_, not called directly from this thread
        // -- every channel_ operation in this codebase only ever runs
        // from strand_ (see Session::sendInputKey's own comment for the
        // same reasoning). self keeps this MicrophoneChannel alive for
        // the posted lambda's duration even if the session is torn down
        // concurrently.
        auto self = shared_from_this();
        boost::asio::post(strand_, [this, self, data, timestamp]() {
            if (!capturing_.load(std::memory_order_acquire)) return;  // closed while this was in flight
            auto promise = aasdk::channel::SendPromise::defer(strand_);
            // No per-chunk success log -- this fires ~100x/sec while
            // the mic is open, matching this codebase's established
            // "don't spam the console for routine high-frequency sends"
            // convention (see session.cpp's ping-log removal).
            promise->then(
                []() {},
                [](const aasdk::error::Error &e) {
                    std::fprintf(stderr, "%s androidauto: microphone data send failed: %s\n", logTimestamp().c_str(),
                                 e.what());
                });
            channel_->sendMediaSourceWithTimestampIndication(timestamp, *data, promise);
        });
    }
}

void MicrophoneChannel::stopCapture() {
    if (!capturing_.exchange(false, std::memory_order_acq_rel)) {
        return;  // wasn't running
    }
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (alsaInput_) {
        alsaInput_->close();
        alsaInput_.reset();
    }
}

void MicrophoneChannel::onMediaChannelAckIndication(
    const aap_protobuf::service::media::source::message::Ack &) {
    channel_->receive(this->shared_from_this());
}

void MicrophoneChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("%s androidauto: microphone channel error: %s\n", logTimestamp().c_str(), e.what());
}

}  // namespace androidauto
