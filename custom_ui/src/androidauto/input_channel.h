#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Channel/InputSource/IInputSourceService.hpp>
#include <aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp>

namespace androidauto {

// Drives aasdk's InputSourceService channel -- accepts the phone's
// ChannelOpenRequest (unconditionally, always OK -- this channel has
// no real reason to refuse), logs KeyBindingRequest (steering-wheel/
// hardware-key mapping -- not handled, this device's touch-only UI
// doesn't have a wheel to bind), and exposes sendTouch() for the app
// to push real touch events once wired to a HAL touch source (see
// docs/IMPLEMENTATION_PLAN.md Phase 2 -- not yet connected to
// src/hal/touch.h; LVGL owns that evdev fd exclusively for the UI's
// own rendering, forwarding touch to Android Auto needs its own
// separate evdev reader, that's the next increment after this one).
//
// The receive()-then-re-arm pattern mirrors Session/BluetoothService
// -- aasdk's InputSourceService.cpp does NOT auto-rearm after
// dispatching a known message, only its unhandled-message-id fallback
// does (confirmed by reading InputSourceService.cpp directly).
//
// service_id numbering caveat: aap_protobuf::service::Service.id
// (used when advertising this channel in ServiceDiscoveryResponse,
// see session.cpp) is filled with
// static_cast<int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE) --
// aasdk's own local C++ enum ordinal, used as a best-available proxy
// for the real AA wire protocol's numeric service ID since no
// authoritative source or captured traffic for this specific value
// was found (docs/logs/ covers the Bluetooth pre-connection dance,
// not an actual aasdk-level session). Worth confirming against a real
// capture if a phone doesn't recognize the advertised channel.
class InputChannel : public aasdk::channel::inputsource::IInputSourceServiceEventHandler,
                      public std::enable_shared_from_this<InputChannel> {
public:
    using Pointer = std::shared_ptr<InputChannel>;

    InputChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger);

    // Arms the channel's receive loop -- call once, right after
    // construction, so it's ready to catch the phone's
    // ChannelOpenRequest whenever it arrives.
    void start();

    // timestamp is microseconds, matching InputReport's own
    // documented-by-usage convention elsewhere in aasdk (not
    // independently confirmed against a real phone).
    void sendTouch(std::uint32_t x, std::uint32_t y, std::uint32_t pointerId,
                   aap_protobuf::service::inputsource::message::PointerAction action,
                   std::uint64_t timestampMicros);

    void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
    void onKeyBindingRequest(const aap_protobuf::service::media::sink::message::KeyBindingRequest &request) override;
    void onChannelError(const aasdk::error::Error &e) override;

private:
    boost::asio::io_service::strand &strand_;
    aasdk::channel::inputsource::IInputSourceService::Pointer channel_;
};

}  // namespace androidauto
