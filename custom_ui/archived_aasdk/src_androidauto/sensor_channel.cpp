#include "androidauto/sensor_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"

#include <aasdk/Channel/SensorSource/SensorSourceService.hpp>
#include <aap_protobuf/service/sensorsource/message/DrivingStatus.pb.h>
#include <aap_protobuf/service/sensorsource/message/DrivingStatusData.pb.h>
#include <aap_protobuf/service/sensorsource/message/NightModeData.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorType.pb.h>

namespace androidauto {

SensorChannel::SensorChannel(boost::asio::io_service::strand &strand,
                              aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(strand, std::move(messenger))) {
}

void SensorChannel::start() {
    channel_->receive(this->shared_from_this());
}

void SensorChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("%s androidauto: sensor channel open request (priority=%d)\n", logTimestamp().c_str(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("%s androidauto: sensor channel open response sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: sensor channel open response send failed: %s\n", logTimestamp().c_str(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void SensorChannel::onSensorStartRequest(
    const aap_protobuf::service::sensorsource::message::SensorRequest &request) {
    std::printf("%s androidauto: sensor start request, type=%d, min_update_period=%lld\n", logTimestamp().c_str(),
                static_cast<int>(request.type()),
                static_cast<long long>(request.min_update_period()));

    aap_protobuf::service::sensorsource::message::SensorStartResponseMessage response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        [this, self = shared_from_this(), request]() {
            std::printf("%s androidauto: sensor start response sent\n", logTimestamp().c_str());

            // DRIVING_STATUS_DATA is the one sensor this channel
            // actually advertises (see this class's header comment) --
            // the phone won't get any data for it until we send at
            // least one indication, so send it right away rather than
            // waiting for a real driving-status source this project
            // doesn't have yet. DRIVE_STATUS_UNRESTRICTED (0) matches
            // this project's existing assumption elsewhere (no gear/
            // speed sensor wired in, see docs/AUDIO_SUBSYSTEM_INVESTIGATION.md
            // and session.h's onNavigationFocusRequest comment on having
            // no native navigation to arbitrate against) -- never
            // reports the car as moving/restricted.
            if (request.type() ==
                aap_protobuf::service::sensorsource::message::SENSOR_DRIVING_STATUS_DATA) {
                aap_protobuf::service::sensorsource::message::SensorBatch batch;
                batch.add_driving_status_data()->set_status(
                    aap_protobuf::service::sensorsource::message::DRIVE_STATUS_UNRESTRICTED);

                auto eventPromise = aasdk::channel::SendPromise::defer(strand_);
                eventPromise->then(
                    []() { std::printf("%s androidauto: driving status sensor event sent\n", logTimestamp().c_str()); },
                    [](const aasdk::error::Error &e) {
                        std::printf("%s androidauto: driving status sensor event send failed: %s\n",
                                    logTimestamp().c_str(), e.what());
                    });
                channel_->sendSensorEventIndication(batch, eventPromise);
            } else if (request.type() ==
                       aap_protobuf::service::sensorsource::message::SENSOR_NIGHT_MODE) {
                // 2026-08-21: mark subscribed and send the CURRENT
                // known value right away (same "won't get data until we
                // send at least one indication" reasoning as
                // DRIVING_STATUS_DATA above) -- setNightMode() takes
                // over sending fresh events on every real change from
                // here on, this is just the initial catch-up.
                nightModeSensorActive_ = true;
                aap_protobuf::service::sensorsource::message::SensorBatch batch;
                batch.add_night_mode_data()->set_night_mode(nightMode_);

                auto eventPromise = aasdk::channel::SendPromise::defer(strand_);
                eventPromise->then(
                    []() { std::printf("%s androidauto: night mode sensor event sent\n", logTimestamp().c_str()); },
                    [](const aasdk::error::Error &e) {
                        std::printf("%s androidauto: night mode sensor event send failed: %s\n",
                                    logTimestamp().c_str(), e.what());
                    });
                channel_->sendSensorEventIndication(batch, eventPromise);
            }
        },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: sensor start response send failed: %s\n", logTimestamp().c_str(), e.what());
        });
    channel_->sendSensorStartResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void SensorChannel::setNightMode(bool nightMode) {
    nightMode_ = nightMode;
    if (!nightModeSensorActive_) {
        return;
    }

    aap_protobuf::service::sensorsource::message::SensorBatch batch;
    batch.add_night_mode_data()->set_night_mode(nightMode_);

    auto eventPromise = aasdk::channel::SendPromise::defer(strand_);
    eventPromise->then(
        []() { std::printf("%s androidauto: night mode sensor event sent\n", logTimestamp().c_str()); },
        [](const aasdk::error::Error &e) {
            std::printf("%s androidauto: night mode sensor event send failed: %s\n",
                        logTimestamp().c_str(), e.what());
        });
    channel_->sendSensorEventIndication(batch, eventPromise);
}

void SensorChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("%s androidauto: sensor channel error: %s\n", logTimestamp().c_str(), e.what());
}

}  // namespace androidauto
