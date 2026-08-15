#include "androidauto/session.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

#include "androidauto/log_timing.h"

#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>
#include <aasdk/Channel/Control/ControlServiceChannel.hpp>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>

namespace androidauto {

namespace {

// PingRequest.timestamp is meant to be a real wall-clock epoch
// timestamp -- but this device has no RTC and no NTP client anywhere
// in its rootfs (no hwclock binary, nothing sets the clock at boot),
// so system_clock::now() reads as whatever the kernel's boot-time
// default is: effectively still near the Unix epoch. Confirmed by
// every hardware log this session showing AASDK's own internal log
// lines stamped "1970-01-01". Found while chasing a long-running
// silent-disconnect bug: the session completes every protocol
// exchange correctly (handshake, ServiceDiscovery, every channel
// opening and completing setup) and then the phone simply stops
// acknowledging pings a few cycles in, followed by a delayed clean
// TCP close -- exactly the kind of symptom a phone-side sanity/
// freshness check on an implausible "January 1970" timestamp would
// produce, with no protocol-content bug to find. Falls back to a
// fixed, roughly-current baseline (updated 2026-08-13 -- doesn't need
// to be exact, just plausible rather than garbage) plus monotonic
// elapsed time, whenever the real clock reads before a sane threshold
// (year 2020). If this device's clock is ever actually set correctly
// (RTC/NTP added later), the real value is used unmodified -- this
// only kicks in for the "clearly never been set" case.
constexpr std::int64_t kSaneEpochMillisThreshold = 1577836800000LL;    // 2020-01-01 UTC
constexpr std::int64_t kFallbackBaselineEpochMillis = 1755043200000LL;  // ~2026-08-13 UTC

std::int64_t plausibleEpochMillis() {
    auto real = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
    if (real >= kSaneEpochMillisThreshold) {
        return real;
    }
    static const auto steadyStart = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - steadyStart)
                       .count();
    return kFallbackBaselineEpochMillis + elapsed;
}

}  // namespace

Session::Session(boost::asio::io_service &ioService)
    : ioService_(ioService), strand_(ioService), pingTimer_(ioService) {
}

void Session::start(aasdk::transport::ITransport::Pointer transport) {
    // 2026-08-12: zero point for every [+Xms] timestamp in this file's
    // logging (see log_timing.h) -- added per explicit request, after a
    // live-debugger capture plan was judged too complicated for what's
    // actually needed: a precise timeline of a single hardware run.
    markSessionStart();

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
                std::printf("[+%ldms] androidauto: touch forwarder failed to start -- "
                             "Android Auto session continues without touch input\n",
                             elapsedMs());
            }
        }
    });

    // 2026-08-12: inputChannel_->start() (i.e. channel_->receive(),
    // arming it for the phone's ChannelOpenRequest) used to happen
    // right here, before the version/handshake/ServiceDiscovery dance
    // even begins. Moved to onServiceDiscoveryRequest(), after
    // sendServiceDiscoveryResponse() actually confirms sent -- matches
    // github.com/mossyhub/openautolink's live_session.cpp exactly,
    // whose own comment at that point reads "NOW start all service
    // handlers -- after TLS + auth + discovery are complete". Only the
    // *construction* of these channel objects (below) stays here;
    // that's just allocating C++ objects, not touching the wire.

    // Video + the three audio sink channels, constructed alongside
    // inputChannel_ -- see session.h's member comment and
    // Session::onServiceDiscoveryRequest() for the matching
    // advertisement and the deferred start() calls. PCM device
    // strings/rates are the real confirmed routes from
    // docs/AUDIO_SUBSYSTEM_INVESTIGATION.md (SYSTEM_AUDIO's
    // plug:softvol4 route is an explicitly-flagged approximation, not
    // an independently confirmed 1:1 mapping -- see that doc).
    videoChannel_ = std::make_shared<VideoChannel>(strand_, messenger);

    audioChannelMedia_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO, "plug:softvol2", 48000, 2);

    audioChannelGuidance_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO, "plug:softvol1", 16000, 1);

    audioChannelSystem_ = std::make_shared<AudioChannel>(
        strand_, messenger, aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO, "plug:softvol4", 16000, 1);

    sensorChannel_ = std::make_shared<SensorChannel>(strand_, messenger);

    microphoneChannel_ = std::make_shared<MicrophoneChannel>(strand_, messenger);

    bluetoothChannel_ = std::make_shared<BluetoothChannel>(strand_, messenger);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: version request sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: version request send failed: %s\n", elapsedMs(), e.what());
        });
    std::printf("[+%ldms] androidauto: AOAP device ready, sending version request...\n", elapsedMs());
    controlChannel_->sendVersionRequest(promise);
}

void Session::continueSSLHandshake() {
    bool active = false;
    try {
        active = cryptor_->doHandshake();
    } catch (const aasdk::error::Error &e) {
        std::printf("[+%ldms] androidauto: SSL handshake failed: %s\n", elapsedMs(), e.what());
        return;
    }
    std::printf("[+%ldms] androidauto: continueSSLHandshake: doHandshake() active=%d\n", elapsedMs(), active);

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
    std::printf("[+%ldms] androidauto: continueSSLHandshake: outBuffer size=%zu\n", elapsedMs(), outBuffer.size());
    // NOTE: opencardev/openauto's onHandshake() sends readHandshakeBuffer()
    // unconditionally whenever doHandshake() isn't yet active (no empty
    // check), only branching to AuthComplete once active -- this
    // function instead gates the send on !outBuffer.empty() and checks
    // `active` separately below, which could in theory send both a
    // (non-empty) handshake buffer AND AuthComplete on the same call if
    // OpenSSL's BIO produces final output bytes in the same step it
    // flips active. Left as-is rather than "fixed" on speculation: this
    // exact logic already completed a real handshake on real hardware
    // (2348 + 51 byte exchange, "SSL handshake complete" printed) before
    // the AuthComplete fix landed, so it's empirically validated for
    // this device's actual flight count -- only the missing AuthComplete
    // after was broken. Flagged here in case a future device/Android
    // version's handshake produces a different flight pattern.
    if (!outBuffer.empty()) {
        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then(
            []() { std::printf("[+%ldms] androidauto: handshake buffer sent\n", elapsedMs()); },
            [](const aasdk::error::Error &e) {
                std::printf("[+%ldms] androidauto: handshake send failed: %s\n", elapsedMs(), e.what());
            });
        controlChannel_->sendHandshake(std::move(outBuffer), promise);
    }

    if (active) {
        std::printf("[+%ldms] androidauto: SSL handshake complete\n", elapsedMs());

        // 2026-08-12 FIX: this was the actual bug behind a real
        // hardware TCP EOF right after "SSL handshake complete" --
        // every channel erroring at once, ServiceDiscoveryRequest never
        // received. Reviewed the reference implementation this whole
        // aasdk lineage descends from (opencardev/openauto,
        // AndroidAutoEntity::onHandshake()) and found it sends a
        // required AuthResponse{status=STATUS_SUCCESS} via
        // sendAuthComplete() the moment the handshake finishes -- a
        // message this class never sent at all. The phone was waiting
        // for that confirmation and closing the connection when it
        // never came.
        aap_protobuf::service::control::message::AuthResponse authComplete;
        authComplete.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);
        auto authPromise = aasdk::channel::SendPromise::defer(strand_);
        authPromise->then(
            []() { std::printf("[+%ldms] androidauto: auth complete sent\n", elapsedMs()); },
            [](const aasdk::error::Error &e) {
                std::printf("[+%ldms] androidauto: auth complete send failed: %s\n", elapsedMs(), e.what());
            });
        controlChannel_->sendAuthComplete(authComplete, authPromise);
    }
}

void Session::onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                                 aap_protobuf::shared::MessageStatus status) {
    std::printf("[+%ldms] androidauto: version response %u.%u, status=%d\n", elapsedMs(), majorCode, minorCode,
                static_cast<int>(status));

    // 2026-08-12: this used to unconditionally proceed to the SSL
    // handshake regardless of status -- found missing by the same
    // reference diff (opencardev/openauto's onVersionResponse()) that
    // caught the missing AuthComplete/AudioFocus/ByeBye/NavFocus
    // responses. Every real hardware run so far reported status=0
    // (STATUS_SUCCESS), so this hasn't caused an observed failure, but
    // attempting a handshake after a real version mismatch would be a
    // confusing, nonsensical failure mode instead of a clear one.
    if (status == aap_protobuf::shared::MessageStatus::STATUS_NO_COMPATIBLE_VERSION) {
        std::printf("[+%ldms] androidauto: version mismatch, not proceeding to handshake\n", elapsedMs());
        return;
    }

    this->continueSSLHandshake();
    controlChannel_->receive(this->shared_from_this());
}

void Session::onHandshake(const aasdk::common::DataConstBuffer &payload) {
    std::printf("[+%ldms] androidauto: handshake payload received (%zu bytes)\n", elapsedMs(), payload.size);
    cryptor_->writeHandshakeBuffer(payload);
    this->continueSSLHandshake();
    controlChannel_->receive(this->shared_from_this());
}

void Session::onServiceDiscoveryRequest(
    const aap_protobuf::service::control::message::ServiceDiscoveryRequest &request) {
    std::printf("[+%ldms] androidauto: service discovery request from '%s'\n", elapsedMs(),
                request.device_name().c_str());

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

    // 2026-08-12: found by diffing against github.com/mossyhub/openautolink's
    // own live_session.cpp (a real, currently-working AA wireless
    // implementation using the exact same aap_protobuf-based aasdk this
    // project vendors) -- its onServiceDiscoveryRequest() explicitly
    // labels this trio "v1.6 protocol fields" (matching this project's
    // own AASDK_MINOR=6, see third_party/aasdk/include/aasdk/Version.hpp)
    // and, distinctly from everything above, ALSO populates every
    // `[deprecated = true]` top-level field in ServiceDiscoveryResponse.proto
    // (make/model/year/vehicle_id/head_unit_make/model/software_build/
    // software_version) alongside the modern headunit_info submessage,
    // with its own comment calling them out as needed "for backward
    // compat" -- i.e. some real phone-side AA versions still validate
    // the legacy fields despite the proto marking them deprecated. This
    // project only ever set headunit_info, never these -- a real,
    // concrete candidate for the "Communication error 2 - incompatible
    // software" screen seen on real hardware (proto2 omits an unset
    // optional field from the wire entirely, so these were completely
    // absent, not just empty).
    response.set_probe_for_support(false);
    response.set_make("custom_ui");
    response.set_model("prado-firmware-reconstruction");
    response.set_year("2026");
    response.set_vehicle_id("prado-custom-ui-001");
    response.set_head_unit_make("custom_ui");
    response.set_head_unit_model("prado-firmware-reconstruction");
    response.set_head_unit_software_build("1");
    response.set_head_unit_software_version("1.0");

    // 2026-08-12: added per opencardev/openauto's own
    // onServiceDiscoveryRequest() -- unlike AuthComplete/AudioFocus
    // Response/ByeByeResponse/NavFocusResponse (all required, found
    // missing by the same reference diff), every field here is
    // `optional` in ServiceDiscoveryResponse.proto, so this is a
    // best-practice match rather than a confirmed-required fix. Values
    // copied from that reference as reasonable defaults, not
    // independently tuned for this hardware.
    auto *pingConfig = response.mutable_connection_configuration()->mutable_ping_configuration();
    pingConfig->set_tracked_ping_count(5);
    pingConfig->set_timeout_ms(3000);
    pingConfig->set_interval_ms(1000);
    pingConfig->set_high_latency_threshold_ms(200);

    auto *inputService = response.add_channels();
    // See input_channel.h's header comment for the service_id-numbering
    // caveat -- this uses aasdk's own ChannelId ordinal as a best-
    // available proxy, not an independently confirmed wire value.
    inputService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE));
    auto *inputSourceService = inputService->mutable_input_source_service();
    auto *touchscreen = inputSourceService->add_touchscreen();
    touchscreen->set_width(800);
    touchscreen->set_height(480);

    // 2026-08-15: the physical control knob (hal/knob.h) is real
    // hardware this device has -- KeyBindingRequest's own reply
    // ("no wheel/hardware keys to bind") only ever meant this project
    // hadn't wired it into the AA session yet, not that the hardware
    // doesn't exist. These are the real AAOS RotaryController keycodes
    // (confirmed against AOSP's own KeyEvent.java, not guessed):
    // KEYCODE_SYSTEM_NAVIGATION_UP/DOWN for rotation ticks,
    // KEYCODE_DPAD_CENTER for the push button. Declaring them here is
    // required -- a keycode InputChannel::sendKey() sends but that
    // isn't listed here may be silently ignored by the phone. See
    // hal/knob.cpp for where these get sent from.
    inputSourceService->add_keycodes_supported(280);  // KEYCODE_SYSTEM_NAVIGATION_UP
    inputSourceService->add_keycodes_supported(281);  // KEYCODE_SYSTEM_NAVIGATION_DOWN
    inputSourceService->add_keycodes_supported(23);   // KEYCODE_DPAD_CENTER

    auto *videoService = response.add_channels();
    videoService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));
    auto *videoSink = videoService->mutable_media_sink_service();
    videoSink->set_available_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    // available_while_in_call and its audio siblings below: found
    // missing by diffing against openautolink's live_session.cpp,
    // which sets this on every media sink service (optional field, so
    // structurally harmless either way, but matches the reference
    // exactly rather than leaving it unset/absent from the wire).
    videoSink->set_available_while_in_call(true);
    auto *videoConfig = videoSink->add_video_configs();
    videoConfig->set_codec_resolution(
        aap_protobuf::service::media::sink::message::VIDEO_800x480);
    videoConfig->set_frame_rate(aap_protobuf::service::media::sink::message::VIDEO_FPS_30);
    videoConfig->set_video_codec_type(
        aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
    // 2026-08-15: found via a real phone-side adb logcat capture --
    // Gearhead rejected the session with "Critical error 2 detail: 21
    // msg: density missing" right before tearing down, immediately
    // after this VideoConfiguration went out with density/real_density
    // left unset (VideoConfiguration.proto fields 5/9, both optional
    // but apparently required in practice by this Gearhead version).
    // 140 matches the real reference's own default (f1xpl/openauto's
    // Configuration.cpp: screenDPI_ = 140), a well-established value
    // for this class of 800x480 automotive display, not a guess.
    videoConfig->set_density(140);
    videoConfig->set_real_density(140);

    auto *mediaAudioService = response.add_channels();
    mediaAudioService->set_id(
        static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO));
    auto *mediaAudioSink = mediaAudioService->mutable_media_sink_service();
    mediaAudioSink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    mediaAudioSink->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA);
    mediaAudioSink->set_available_while_in_call(true);
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
    guidanceAudioSink->set_available_while_in_call(true);
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
    systemAudioSink->set_available_while_in_call(true);
    auto *systemAudioConfig = systemAudioSink->add_audio_configs();
    systemAudioConfig->set_sampling_rate(16000);
    systemAudioConfig->set_number_of_bits(16);
    systemAudioConfig->set_number_of_channels(1);

    // 2026-08-12: found by diffing against openautolink's
    // live_session.cpp -- this project advertised NO sensor channel at
    // all. Real Android Auto phone-app versions are widely known to
    // require at least SENSOR_DRIVING_STATUS_DATA before staying
    // connected, not just as a nice-to-have -- its total absence fits
    // this project's exact real-hardware symptom (structurally valid
    // ServiceDiscoveryResponse accepted, then every channel drops
    // together a few seconds later). See sensor_channel.h for why only
    // this one sensor is advertised (no real source for any others).
    auto *sensorService = response.add_channels();
    sensorService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::SENSOR));
    sensorService->mutable_sensor_source_service()->add_sensors()->set_sensor_type(
        aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA);

    // 2026-08-12: found missing by a fresh-eyes subagent review chasing
    // the real hardware bug where the phone engages cleanly (handshake,
    // ServiceDiscovery, ping, an early AudioFocusRequest) then silently
    // closes the TCP connection ~800ms later without ever opening a
    // channel or sending ByeByeRequest. This project never advertised
    // ANY MediaSourceService channel at all -- openautolink's
    // live_session.cpp advertises MEDIA_SOURCE_MICROPHONE as a
    // first-class channel, and the real stock libAndroidAuto.so on
    // this exact hardware has a full AudioSource endpoint class (see
    // project_stock_libandroidauto_reference.md). See
    // microphone_channel.h for why this is structural-only (no real
    // mic capture wired in yet). 16kHz/16-bit/mono matches the
    // reference's own config.
    auto *micService = response.add_channels();
    micService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE));
    auto *micSource = micService->mutable_media_source_service();
    micSource->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    auto *micConfig = micSource->mutable_audio_config();
    micConfig->set_sampling_rate(16000);
    micConfig->set_number_of_bits(16);
    micConfig->set_number_of_channels(1);

    // 2026-08-12: found by reviewing github.com/vteckz/MicStream's
    // vendored copy of the ORIGINAL upstream f1x/openauto -- its
    // ServiceFactory constructs a BluetoothService unconditionally for
    // wireless sessions specifically (this project's session is
    // wireless-only). See bluetooth_channel.h for why car_address is
    // deliberately left empty / pairing method UNAVAILABLE rather than
    // trying to source this project's real BT MAC here -- that's a
    // real, supported fallback path in the reference itself, not a
    // guess.
    auto *bluetoothService = response.add_channels();
    bluetoothService->set_id(static_cast<std::int32_t>(aasdk::messenger::ChannelId::BLUETOOTH));
    auto *bluetooth = bluetoothService->mutable_bluetooth_service();
    bluetooth->set_car_address("");
    bluetooth->add_supported_pairing_methods(
        aap_protobuf::service::bluetooth::message::BLUETOOTH_PAIRING_UNAVAILABLE);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this, self = shared_from_this()]() {
            std::printf("[+%ldms] androidauto: service discovery response sent\n", elapsedMs());

            // 2026-08-12: only now -- confirmed sent, not just enqueued
            // -- do the other channels arm receive() for the phone's
            // ChannelOpenRequest, matching openautolink's
            // live_session.cpp exactly (see this function's own
            // comment in Session::start()). Previously these started
            // immediately in Session::start(), before the phone had
            // even sent VersionRequest.
            //
            // 2026-08-12: explicit per-channel confirmation logs added
            // -- this whole block used to run silently, so a hardware
            // log could never confirm these actually executed (as
            // opposed to, say, an exception unwinding through this
            // lambda before reaching later lines).
            videoChannel_->start();
            std::printf("[+%ldms] androidauto: video channel armed\n", elapsedMs());
            audioChannelMedia_->start();
            audioChannelGuidance_->start();
            audioChannelSystem_->start();
            std::printf("[+%ldms] androidauto: audio channels armed\n", elapsedMs());
            inputChannel_->start();
            std::printf("[+%ldms] androidauto: input channel armed\n", elapsedMs());
            sensorChannel_->start();
            std::printf("[+%ldms] androidauto: sensor channel armed\n", elapsedMs());
            microphoneChannel_->start();
            std::printf("[+%ldms] androidauto: microphone channel armed\n", elapsedMs());
            bluetoothChannel_->start();
            std::printf("[+%ldms] androidauto: bluetooth channel armed\n", elapsedMs());

            // Same reference: sends the first ping immediately (not
            // after waiting a full interval_ms) then falls into the
            // regular schedule.
            sendPing();
            schedulePing();
        },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: service discovery response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    controlChannel_->sendServiceDiscoveryResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::sendPing() {
    auto timestamp = plausibleEpochMillis();
    aap_protobuf::service::control::message::PingRequest request;
    request.set_timestamp(timestamp);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        // 2026-08-15: the routine "ping request sent" success log
        // (added 2026-08-12 for a since-resolved investigation into
        // whether the connection survived past pings at all) removed
        // per explicit request -- ping/pong is confirmed stable and
        // fires every ~1s, flooding the console for no remaining
        // diagnostic value. Failure still logged; that's the case that
        // actually matters now.
        []() {},
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: ping request send failed: %s\n", elapsedMs(), e.what());
        });
    controlChannel_->sendPingRequest(request, promise);
}

void Session::schedulePing() {
    if (stopping_) {
        return;
    }
    pingTimer_.expires_from_now(std::chrono::milliseconds(1000));
    auto self = shared_from_this();
    pingTimer_.async_wait(strand_.wrap([this, self](const boost::system::error_code &ec) {
        if (ec || stopping_) {
            return;  // cancelled (session stopping) -- not an error to report
        }
        sendPing();
        schedulePing();
    }));
}

void Session::onAudioFocusRequest(
    const aap_protobuf::service::control::message::AudioFocusRequest &request) {
    std::printf("[+%ldms] androidauto: audio focus request, type=%d\n", elapsedMs(),
                static_cast<int>(request.audio_focus_type()));

    // 2026-08-12: this used to just log and re-arm receive(), never
    // replying at all -- found missing by the same reference diff that
    // caught the missing AuthComplete send (opencardev/openauto's
    // AndroidAutoEntity::onAudioFocusRequest()).
    //
    // 2026-08-12 REVISED: the GAIN/LOSS-only mapping above collapsed
    // GAIN_TRANSIENT and GAIN_TRANSIENT_MAY_DUCK into plain GAIN --
    // found by diffing against github.com/mossyhub/openautolink's
    // live_session.cpp (HeadlessAutoEntity::onAudioFocusRequest()),
    // which keeps GAIN_TRANSIENT/GAIN_TRANSIENT_MAY_DUCK distinct as
    // AUDIO_FOCUS_STATE_GAIN_TRANSIENT. Matters for correct behavior on
    // the phone side (a transient/may-duck request is meant to be
    // brief and coexist with other audio, e.g. turn-by-turn guidance
    // over media -- collapsing it to a plain GAIN told the phone we'd
    // taken full, non-transient focus instead).
    aap_protobuf::service::control::message::AudioFocusStateType state;
    switch (request.audio_focus_type()) {
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN;
            break;
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN_TRANSIENT:
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
            break;
        case aap_protobuf::service::control::message::AUDIO_FOCUS_RELEASE:
        default:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_LOSS;
            break;
    }

    aap_protobuf::service::control::message::AudioFocusNotification response;
    response.set_focus_state(state);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: audio focus response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: audio focus response send failed: %s\n", elapsedMs(), e.what());
        });
    controlChannel_->sendAudioFocusResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onByeByeRequest(const aap_protobuf::service::control::message::ByeByeRequest &) {
    std::printf("[+%ldms] androidauto: bye-bye request\n", elapsedMs());

    // 2026-08-12: this used to just log and re-arm receive(), never
    // replying -- same missing-response class of bug as
    // onAudioFocusRequest above. The phone (or head unit) initiated a
    // clean shutdown; sendShutdownResponse() is the required
    // acknowledgement (opencardev/openauto's onByeByeRequest()).
    // Deliberately NOT re-arming controlChannel_->receive() afterwards
    // -- the session is ending, there's nothing further to wait for on
    // this channel, matching the reference's own behavior of not
    // calling receive() again here either.
    aap_protobuf::service::control::message::ByeByeResponse response;
    auto promise = aasdk::channel::SendPromise::defer(strand_);
    // 2026-08-14 FIX: same root cause as onChannelError()'s own comment --
    // WirelessSessionManager::run() blocks inside ioService.run() for the
    // whole session and only reaches its post-run() Failed transition
    // once that call actually returns, which never happened on a clean
    // bye-bye either (this handler set stopping_ but never stopped the
    // io_service). Stopped from INSIDE the send promise's own callbacks,
    // not immediately after enqueueing the send, so the ByeByeResponse
    // actually goes out over the wire first -- stopping the io_service
    // any earlier risks cutting off the in-flight write before its
    // completion handler ever runs.
    auto self = shared_from_this();
    promise->then(
        [this, self]() {
            std::printf("[+%ldms] androidauto: bye-bye response sent\n", elapsedMs());
            ioService_.stop();
        },
        [this, self](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: bye-bye response send failed: %s\n", elapsedMs(), e.what());
            ioService_.stop();
        });
    controlChannel_->sendShutdownResponse(response, promise);

    stopping_ = true;
    pingTimer_.cancel();
}

void Session::onByeByeResponse(const aap_protobuf::service::control::message::ByeByeResponse &) {
    std::printf("[+%ldms] androidauto: bye-bye response\n", elapsedMs());
    controlChannel_->receive(this->shared_from_this());
}

void Session::onBatteryStatusNotification(
    const aap_protobuf::service::control::message::BatteryStatusNotification &) {
    controlChannel_->receive(this->shared_from_this());
}

void Session::onNavigationFocusRequest(
    const aap_protobuf::service::control::message::NavFocusRequestNotification &) {
    // 2026-08-12: this used to just re-arm receive() without replying --
    // same missing-response class of bug as onAudioFocusRequest/
    // onByeByeRequest above. NAV_FOCUS_PROJECTED unconditionally matches
    // opencardev/openauto's onNavigationFocusRequest() -- this app has
    // no native navigation of its own to arbitrate against, same
    // reasoning as that reference's own comment.
    aap_protobuf::service::control::message::NavFocusNotification response;
    response.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: navigation focus response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: navigation focus response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    controlChannel_->sendNavigationFocusResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onVoiceSessionRequest(
    const aap_protobuf::service::control::message::VoiceSessionNotification &request) {
    // 2026-08-14: this used to just log-free-and-rearm without ever
    // replying -- found via a systematic audit of every
    // IControlServiceChannel send method against what this class
    // actually calls, prompted by real hardware catching the identical
    // missing-response bug class on KeyBindingRequest (see that
    // handler's own comment -- every channel dying together, 44ms after
    // an unanswered required message). IControlServiceChannel::
    // sendVoiceSessionFocusResponse() exists and was never called.
    // VoiceSessionNotification is used as both the phone's own
    // request AND the required response, carrying a single
    // start/end status -- echoing the same status back is the
    // simplest, least-speculative acknowledgement (no confirmed
    // reference/capture for this specific message to match against,
    // unlike AudioFocus/NavFocus which had real diffs to work from).
    std::printf("[+%ldms] androidauto: voice session request, status=%d\n", elapsedMs(),
                static_cast<int>(request.status()));

    aap_protobuf::service::control::message::VoiceSessionNotification response;
    response.set_status(request.status());

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: voice session response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: voice session response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    controlChannel_->sendVoiceSessionFocusResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onPingRequest(const aap_protobuf::service::control::message::PingRequest &request) {
    // Confirmed protocol contract (IControlServiceChannel::
    // sendPingResponse), not speculative -- the phone uses ping/pong
    // as a keep-alive; echoing the timestamp back is the whole
    // contract, per PingResponse's own single required field.
    //
    // 2026-08-15: the routine "request received"/"response sent"
    // success logs (added 2026-08-12 to confirm ping/pong was actually
    // happening bidirectionally, and later used to determine this
    // field isn't usable for wall-clock time -- both questions long
    // since settled) removed per explicit request -- this fires every
    // ~1s in a stable session and was flooding the console for no
    // remaining diagnostic value. Failure still logged.
    (void)request;

    aap_protobuf::service::control::message::PingResponse response;
    response.set_timestamp(request.timestamp());

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: ping response send failed: %s\n", elapsedMs(), e.what());
        });
    controlChannel_->sendPingResponse(response, promise);

    controlChannel_->receive(this->shared_from_this());
}

void Session::onPingResponse(const aap_protobuf::service::control::message::PingResponse &) {
    // 2026-08-15: routine "response received" success log (added
    // 2026-08-12 to rule out a head-unit-ping-timeout theory, long
    // since settled) removed per explicit request -- same console-
    // flood reasoning as onPingRequest()'s own comment.
    controlChannel_->receive(this->shared_from_this());
}

void Session::sendInputKey(std::uint32_t keycode) {
    // Called from WirelessSessionManager::sendInputKey(), itself
    // called from the sidecar's own socket-connection thread handling
    // a "KEY <code>" command -- a completely different thread from
    // this session's own strand_. Every other channel/send operation
    // in this class only ever runs from within strand_ (posted there
    // by aasdk's own async machinery); posting through it here too,
    // rather than calling inputChannel_->sendKey() directly from the
    // caller's thread, keeps that invariant instead of introducing the
    // one exception.
    auto self = shared_from_this();
    boost::asio::post(strand_, [this, self, keycode]() {
        if (!inputChannel_) return;
        inputChannel_->sendKey(keycode);
    });
}

void Session::onChannelError(const aasdk::error::Error &e) {
    // 2026-08-14 FIX: this used to only set stopping_ and cancel the ping
    // timer -- it never called ioService_.stop(). WirelessSessionManager::
    // run() (wireless_session_manager.cpp) blocks inside ioService.run()
    // for the whole session's lifetime and only reaches its own
    // setStatus(Failed, "Session ended (io_service stopped)") line AFTER
    // that call returns -- but boost::asio's io_service only returns once
    // there's no outstanding async work at all, and this session's other
    // channels each independently re-arm their own receive() after every
    // message, so plenty of "work" was always still outstanding even
    // after the transport itself had already died. Real hardware
    // consequence, caught live: after any real session drop, the
    // WirelessSessionManager thread hung forever inside ioService.run(),
    // state_ stayed stuck at Connected permanently, and -- combined with
    // start()'s own (correct, separately-added) guard against tearing
    // down an already-active session -- EVERY subsequent +AAPDEV=
    // auto-trigger was silently ignored ("a session is already active")
    // against a session that was actually long dead. The user's own
    // words: "says wireless session already active but the phone isn't
    // reconnecting the wifi" -- this is why.
    //
    // OPERATION_ABORTED is aasdk's normal signal that a pending
    // read/write was cancelled (e.g. the transport/io_service is
    // shutting down) -- opencardev/openauto's own onChannelError()
    // treats it as expected-during-stop, not a real failure. Still stops
    // the io_service either way -- an aborted operation on the control
    // channel means this session is ending one way or another, and
    // WirelessSessionManager needs to know that regardless of which
    // branch got there.
    if (e.getCode() == aasdk::error::ErrorCode::OPERATION_ABORTED) {
        std::printf("[+%ldms] androidauto: control channel: operation aborted (expected during "
                    "shutdown): %s\n", elapsedMs(), e.what());
        stopping_ = true;
        pingTimer_.cancel();
        ioService_.stop();
        return;
    }
    std::printf("[+%ldms] androidauto: control channel error: %s\n", elapsedMs(), e.what());
    stopping_ = true;
    pingTimer_.cancel();
    ioService_.stop();
}

}  // namespace androidauto
