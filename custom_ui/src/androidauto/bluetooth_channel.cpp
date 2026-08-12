#include "androidauto/bluetooth_channel.h"

#include <cstdio>

#include "androidauto/log_timing.h"

#include <aasdk/Channel/Bluetooth/BluetoothService.hpp>
#include <aap_protobuf/service/bluetooth/message/BluetoothPairingResponse.pb.h>

namespace androidauto {

BluetoothChannel::BluetoothChannel(boost::asio::io_service::strand &strand,
                                    aasdk::messenger::IMessenger::Pointer messenger)
    : strand_(strand),
      channel_(std::make_shared<aasdk::channel::bluetooth::BluetoothService>(strand, std::move(messenger))) {
}

void BluetoothChannel::start() {
    channel_->receive(this->shared_from_this());
}

void BluetoothChannel::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest &request) {
    std::printf("[+%ldms] androidauto: bluetooth channel open request (priority=%d)\n", elapsedMs(),
                request.priority());

    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: bluetooth channel open response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: bluetooth channel open response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendChannelOpenResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void BluetoothChannel::onBluetoothPairingRequest(
    const aap_protobuf::service::bluetooth::message::BluetoothPairingRequest &request) {
    // Real pairing already happened before this session existed at all
    // (blueware's own AT-command stack, outside aasdk entirely -- see
    // this class's header comment) -- always report success/already
    // paired rather than attempting a real pairing handshake through
    // this channel.
    std::printf("[+%ldms] androidauto: bluetooth pairing request from %s\n", elapsedMs(),
                request.phone_address().c_str());

    aap_protobuf::service::bluetooth::message::BluetoothPairingResponse response;
    response.set_status(aap_protobuf::shared::MessageStatus::STATUS_SUCCESS);
    response.set_already_paired(true);

    auto promise = aasdk::channel::SendPromise::defer(strand_);
    promise->then(
        []() { std::printf("[+%ldms] androidauto: bluetooth pairing response sent\n", elapsedMs()); },
        [](const aasdk::error::Error &e) {
            std::printf("[+%ldms] androidauto: bluetooth pairing response send failed: %s\n", elapsedMs(),
                        e.what());
        });
    channel_->sendBluetoothPairingResponse(response, promise);

    channel_->receive(this->shared_from_this());
}

void BluetoothChannel::onBluetoothAuthenticationResult(
    const aap_protobuf::service::bluetooth::message::BluetoothAuthenticationResult &request) {
    std::printf("[+%ldms] androidauto: bluetooth authentication result, status=%d\n", elapsedMs(),
                static_cast<int>(request.status()));
    channel_->receive(this->shared_from_this());
}

void BluetoothChannel::onChannelError(const aasdk::error::Error &e) {
    std::printf("[+%ldms] androidauto: bluetooth channel error: %s\n", elapsedMs(), e.what());
}

}  // namespace androidauto
