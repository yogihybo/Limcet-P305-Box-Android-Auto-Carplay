#include "androidauto/input_channel.h"

#include <cstdio>

#include <aasdk/Channel/InputSource/InputSourceService.hpp>

namespace androidauto {

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

void InputChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("androidauto: input channel open request (priority=%d)\n", request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("androidauto: input channel open response sent\n"); },
        [](const aasdk::error::Error &e) {
            std::printf("androidauto: input channel open response send failed: %s\n", e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void InputChannel::onKeyBindingRequest(
    const aap_protobuf::service::media::sink::message::KeyBindingRequest &) {
    std::printf("androidauto: key binding request (not handled -- touch-only UI, no wheel)\n");
    channel_->receive(this->shared_from_this());
}

void InputChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("androidauto: input channel error: %s\n", e.what());
}

}  // namespace androidauto
