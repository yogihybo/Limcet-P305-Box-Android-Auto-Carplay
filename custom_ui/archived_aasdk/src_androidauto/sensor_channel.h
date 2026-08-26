#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceService.hpp>
#include <aasdk/Channel/SensorSource/ISensorSourceServiceEventHandler.hpp>

namespace androidauto {

// Drives aasdk's SensorSourceService channel -- advertises
// SENSOR_DRIVING_STATUS_DATA in ServiceDiscoveryResponse (see
// session.cpp), the one sensor real AA phone-app versions are widely
// known to require before staying connected at all, not just a nice-
// to-have like GPS/speed/gear. This project has no real source for any
// of those other sensors (no gear/speed/fuel signal wired into this
// session), so this channel deliberately doesn't advertise them either
// -- advertising a sensor this class can't actually service would be
// worse than not advertising it (same reasoning as input_channel.h's
// touch-only channel), see this class's .cpp for what happens once the
// phone's SensorStartRequest for it arrives.
//
// 2026-08-21: also advertises SENSOR_NIGHT_MODE -- unlike the other
// unadvertised sensors above, this one DOES have a real source now: the
// Limcet MCU's headlight signal (hal::mcu_input.h's own
// get_night_mode(), custom_ui's process, forwarded cross-process via
// hal::AndroidAutoClient::sendNightMode() -> WirelessSessionManager::
// sendNightMode() -> Session::sendNightMode() -> setNightMode() below).
//
// 2026-08-12: added while chasing a real "Communication error 2 -
// incompatible software" / every-channel-drops-together-after-
// ServiceDiscovery failure on real hardware. Not confirmed as THE fix
// (found by diffing against github.com/mossyhub/openautolink's
// live_session.cpp, which advertises a full sensor list including this
// one) -- a real, buildable gap either way, since a phone that DOES
// require DRIVING_STATUS_DATA before it'll stay connected would fail
// in exactly this observed way (accepts ServiceDiscoveryResponse
// structurally, then drops the whole session at the app layer once it
// notices the required sensor never showed up). Not yet hardware-tested.
class SensorChannel : public aasdk::channel::sensorsource::ISensorSourceServiceEventHandler,
                       public std::enable_shared_from_this<SensorChannel> {
public:
    using Pointer = std::shared_ptr<SensorChannel>;

    SensorChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger);

    // Arms the channel's receive loop -- call once, right after
    // construction (deferred until after ServiceDiscoveryResponse is
    // confirmed sent, same as video/audio/input -- see session.cpp).
    void start();

    void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
    void onSensorStartRequest(const aap_protobuf::service::sensorsource::message::SensorRequest &request) override;
    void onChannelError(const aasdk::error::Error &e) override;

    // Updates the current night-mode value and, if the phone has
    // already subscribed to SENSOR_NIGHT_MODE (sent a SensorStartRequest
    // for it), immediately sends a fresh NightModeData event -- same
    // "the phone won't get any data for it until we send an indication"
    // reasoning already established for DRIVING_STATUS_DATA in
    // onSensorStartRequest()'s own comment, just proactive instead of
    // only-on-request since this value can genuinely change mid-session
    // (headlights on/off), unlike driving status. Safe to call whether
    // or not the phone has subscribed yet, or whether this channel has
    // even been start()'d -- just updates the stored value either way,
    // matching the SessionMutex-guarded no-op-if-no-session contract
    // its own caller chain (Session::sendNightMode()) already provides.
    void setNightMode(bool nightMode);

private:
    boost::asio::io_service::strand &strand_;
    aasdk::channel::sensorsource::ISensorSourceService::Pointer channel_;

    bool nightMode_ = false;
    bool nightModeSensorActive_ = false;
};

}  // namespace androidauto
