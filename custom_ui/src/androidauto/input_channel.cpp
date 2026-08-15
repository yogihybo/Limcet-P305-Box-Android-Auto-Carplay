#include "androidauto/input_channel.h"

#include <cstdio>
#include <ctime>

#include "androidauto/log_timing.h"

#include <aasdk/Channel/InputSource/InputSourceService.hpp>

namespace androidauto {

namespace {
// Same CLOCK_MONOTONIC-microseconds convention as
// TouchForwarder::nowMicros() -- InputReport.timestamp doesn't need to
// be wall-clock (see this project's own PingRequest.timestamp
// investigation for why that field specifically was never usable for
// real time), just monotonically increasing.
std::uint64_t nowMicros() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}
}  // namespace

InputChannel::InputChannel(boost::asio::io_service::strand &strand,
                            aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::inputsource::InputSourceService>(strand, std::move(messenger))) {
}

void InputChannel::start() {
    channel_->receive(this->shared_from_this());
}

void InputChannel::sendTouch(std::uint32_t x, std::uint32_t y, std::uint32_t pointerId,
                              aap_protobuf::service::inputsource::message::PointerAction action,
                              std::uint64_t timestampMicros) {
    aap_protobuf::service::inputsource::message::InputReport report;
    report.set_timestamp(timestampMicros);
    auto *touchEvent = report.mutable_touch_event();
    touchEvent->set_action(action);
    auto *pointer = touchEvent->add_pointer_data();
    pointer->set_x(x);
    pointer->set_y(y);
    pointer->set_pointer_id(pointerId);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() {},
        [](const aasdk::error::Error &e) {
            std::printf("androidauto: input report send failed: %s\n", e.what());
        });
    channel_->sendInputReport(report, promise);
}

void InputChannel::sendKey(std::uint32_t keycode) {
    for (bool down : {true, false}) {
        aap_protobuf::service::inputsource::message::InputReport report;
        report.set_timestamp(nowMicros());
        auto *key = report.mutable_key_event()->add_keys();
        key->set_keycode(keycode);
        key->set_down(down);
        key->set_metastate(0);

        auto promise = aasdk::channel::SendPromise::defer(strand_);
        promise->then(
            []() {},
            [keycode](const aasdk::error::Error &e) {
                std::printf("androidauto: key event send failed (keycode=%u): %s\n", keycode, e.what());
            });
        channel_->sendInputReport(report, promise);
    }
}

void InputChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("%s androidauto: input channel open request (priority=%d)\n", logTimestamp().c_str(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: input channel open response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: input channel open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void InputChannel::onKeyBindingRequest(
    const aap_protobuf::service::media::sink::message::KeyBindingRequest &) {
    // 2026-08-14: this used to just log and re-arm receive() without ever
    // replying -- the exact same class of bug as every other missing-
    // response gap found and fixed this whole session (AuthComplete,
    // AudioFocusResponse, ByeByeResponse, NavigationFocusResponse -- see
    // project_aa_missing_auth_complete.md). Real hardware log evidence: a
    // KeyBindingRequest arrived at +419ms, logged as "not handled", and
    // every single channel died with the identical AASDK Error 33 (TCP
    // eof) signature 44ms later -- the same simultaneous-full-session-
    // drop pattern this project has chased all session, now caught live
    // for the first time with a specific message right before it.
    // KeyBindingResponse's only field is a required status -- replying
    // STATUS_SUCCESS (this device genuinely has no wheel/hardware keys to
    // bind, but structurally answering unconditionally is the same
    // pattern already proven correct for every other required response
    // in this codebase) rather than leaving the phone waiting on a reply
    // that was never coming.
    std::printf("%s androidauto: key binding request (no wheel/hardware keys to bind -- "
                "replying success)\n", logTimestamp().c_str());

    aap_protobuf::service::media::sink::message::KeyBindingResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: key binding response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: key binding response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendKeyBindingResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void InputChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("%s androidauto: input channel error: %s\n", logTimestamp().c_str(), e.what());
}

}  // namespace androidauto
