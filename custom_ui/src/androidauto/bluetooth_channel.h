// The Android Auto BLUETOOTH channel -- this project's
// ServiceDiscoveryResponse never advertised it at all until now.
// Found 2026-08-12 while reviewing github.com/vteckz/MicStream's
// vendored copy of the ORIGINAL upstream f1x/openauto (the real
// project every other reference in this codebase's history descends
// from -- opencardev/openauto, mossyhub/openautolink). Its
// ServiceFactory::create() constructs a BluetoothService
// UNCONDITIONALLY whenever wireless projection is enabled -- this
// project's session is wireless-only, so this is exactly that case.
// Also matches [[project_stock_libandroidauto_reference]]'s
// BluetoothEndpoint class confirmed present in the real stock
// libAndroidAuto.so on this exact hardware.
//
// Deliberately does NOT try to renegotiate real Bluetooth pairing
// through this channel -- this project's actual classic-Bluetooth
// pairing already happens entirely outside the aasdk session, via
// blueware's own AT-command stack (hal/bluetooth.cpp) as part of the
// BW_AAP wireless handoff that gets a session this far in the first
// place (see bw_aap_client.h/wireless_session_manager.h). By the time
// any aasdk Session exists at all, the phone is necessarily already
// bonded. Wiring a real car_address into this channel would mean the
// androidauto-sidecar process (separate from custom_ui, no shared
// hal:: access -- see androidauto_client.h's own header comment on
// that split) opening /dev/bw_serial itself and querying AT+ADDR,
// risking real contention with custom_ui's own concurrent use of the
// same serial port. The upstream reference itself has an equally real,
// supported fallback for exactly this "no local adapter wired in"
// case -- empty car_address + BLUETOOTH_PAIRING_UNAVAILABLE -- used
// here instead. onBluetoothPairingRequest still answers gracefully
// (STATUS_SUCCESS, already_paired=true) in case a phone tries anyway
// despite the advertised capability, since pairing has, in fact,
// already succeeded by construction.
#pragma once

#include <memory>

#include <boost/asio.hpp>

#include <aasdk/Messenger/IMessenger.hpp>
#include <aasdk/Channel/Bluetooth/IBluetoothService.hpp>
#include <aasdk/Channel/Bluetooth/IBluetoothServiceEventHandler.hpp>

namespace androidauto {

class BluetoothChannel : public aasdk::channel::bluetooth::IBluetoothServiceEventHandler,
                          public std::enable_shared_from_this<BluetoothChannel> {
public:
    using Pointer = std::shared_ptr<BluetoothChannel>;

    BluetoothChannel(boost::asio::io_service::strand &strand, aasdk::messenger::IMessenger::Pointer messenger);

    // Arms the channel's receive loop -- call once, right after
    // construction (deferred until after ServiceDiscoveryResponse is
    // confirmed sent, same as every other channel -- see session.cpp).
    void start();

    void onChannelOpenRequest(const aap_protobuf::service::control::message::ChannelOpenRequest &request) override;
    void onBluetoothPairingRequest(
        const aap_protobuf::service::bluetooth::message::BluetoothPairingRequest &request) override;
    void onBluetoothAuthenticationResult(
        const aap_protobuf::service::bluetooth::message::BluetoothAuthenticationResult &request) override;
    void onChannelError(const aasdk::error::Error &e) override;

private:
    boost::asio::io_service::strand &strand_;
    aasdk::channel::bluetooth::IBluetoothService::Pointer channel_;
};

}  // namespace androidauto
