// Standalone test for the raw BlueZ RFCOMM socket layer
// (BluetoothRFCOMMTransport + accept_rfcomm_connection) -- NOT a full
// Android Auto session. Listens for a connection on the given RFCOMM
// channel, then reads and hex-dumps whatever bytes arrive for a fixed
// window. Proves the socket plumbing works; does not attempt the
// aasdk Messenger/Channel handshake on top of it yet (see
// bluetooth_transport.h for why that's deliberately deferred).
//
// Without a real phone (which needs SDP discovery this doesn't
// implement yet -- see bluetooth_rfcomm_server.h), test this locally:
// pair this device with another Linux machine, then from that machine
// run `rfcomm connect hci0 <this-device-bdaddr> <channel>` and type
// some bytes into the resulting /dev/rfcommN, or use `bluetoothctl`'s
// own connect tooling.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

#include <boost/asio.hpp>

#include "androidauto/bluetooth_rfcomm_server.h"
#include "androidauto/bluetooth_transport.h"

int main(int argc, char **argv) {
    std::uint8_t channel = argc > 1 ? static_cast<std::uint8_t>(std::atoi(argv[1])) : 4;
    int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

    int fd = androidauto::accept_rfcomm_connection(channel);
    if (fd < 0) {
        return 1;
    }

    boost::asio::io_service ioService;
    auto transport = std::make_shared<androidauto::BluetoothRFCOMMTransport>(ioService, fd);

    std::function<void()> readMore = [&]() {
        auto promise = aasdk::transport::ITransport::ReceivePromise::defer(ioService);
        promise->then(
            [&readMore](const aasdk::common::Data &data) {
                std::printf("androidauto: received %zu bytes:", data.size());
                for (auto byte : data) {
                    std::printf(" %02x", byte);
                }
                std::printf("\n");
                readMore();
            },
            [](const aasdk::error::Error &e) {
                std::printf("androidauto: RFCOMM transport error: %s\n", e.what());
            });
        transport->receive(256, promise);
    };
    readMore();

    boost::asio::steady_timer stopTimer(ioService, std::chrono::seconds(seconds));
    stopTimer.async_wait([&](const boost::system::error_code &) {
        transport->stop();
        ioService.stop();
    });

    ioService.run();
    return 0;
}
