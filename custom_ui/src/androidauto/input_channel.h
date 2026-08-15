#pragma once

#include <functional>
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
// to push real touch events. Now wired to a real source --
// src/androidauto/touch_forwarder.h (TouchForwarder) opens its own,
// second evdev fd (independent of src/hal/touch.h, which LVGL owns
// exclusively for the UI's own rendering) and calls sendTouch()
// directly. See setChannelOpenCallback() below for how Session gates
// TouchForwarder::start() on this channel actually being open.
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

    // Invoked once, synchronously, from onChannelOpenRequest() -- the
    // signal that the phone has actually opened this channel and is
    // ready to receive InputReports. Session uses this to gate
    // TouchForwarder::start() (see session.cpp): sending touch events
    // before the phone has opened the channel isn't a documented-valid
    // aasdk sequence, and there's no reason to have a second evdev fd
    // open before it's needed. Fired before the async
    // ChannelOpenResponse send even completes -- fine, since all this
    // does is arm a local read loop, it doesn't send anything over the
    // channel itself. Not called again on subsequent opens (aasdk
    // doesn't appear to re-request an already-open channel, and this
    // class doesn't track re-open semantics either way).
    void setChannelOpenCallback(std::function<void()> callback);

    // timestamp is microseconds, matching InputReport's own
    // documented-by-usage convention elsewhere in aasdk (not
    // independently confirmed against a real phone).
    void sendTouch(std::uint32_t x, std::uint32_t y, std::uint32_t pointerId,
                   aap_protobuf::service::inputsource::message::PointerAction action,
                   std::uint64_t timestampMicros);

    // Sends a momentary key tap -- a down=true InputReport immediately
    // followed by a down=false one, matching the physical control
    // knob's own MCU-relayed event shape (hal/mcu_input.h: rotation
    // ticks and the push-button's press edge are both momentary
    // events, never a sustained "held" state this class needs to
    // track). keycode is a real Android KeyEvent constant -- see
    // hal/knob.cpp's own comment for which ones and why
    // (KEYCODE_SYSTEM_NAVIGATION_UP/DOWN=280/281 for rotation,
    // KEYCODE_DPAD_CENTER=23 for the push button -- the real AAOS
    // RotaryController convention, confirmed against AOSP's own
    // KeyEvent.java, not guessed). Must also be listed in this
    // channel's own keycodes_supported (session.cpp's
    // ServiceDiscoveryResponse) or the phone may silently ignore it.
    void sendKey(std::uint32_t keycode);

    void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
    void onKeyBindingRequest(const aap_protobuf::service::media::sink::message::KeyBindingRequest &request) override;
    void onChannelError(const aasdk::error::Error &e) override;

private:
    boost::asio::io_service::strand &strand_;
    aasdk::channel::inputsource::IInputSourceService::Pointer channel_;
    std::function<void()> channelOpenCallback_;
};

}  // namespace androidauto
