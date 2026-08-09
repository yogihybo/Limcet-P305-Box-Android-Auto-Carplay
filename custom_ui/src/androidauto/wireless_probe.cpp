#include "androidauto/wireless_probe.h"

#include <cstdio>
#include <memory>

#include <boost/asio.hpp>

#include <aasdk/TCP/TCPWrapper.hpp>
#include <aasdk/TCP/TCPEndpoint.hpp>
#include <aasdk/Transport/TCPTransport.hpp>

#include "androidauto/session.h"

namespace androidauto {

bool run_wireless_probe(const std::string &host, std::uint16_t port, int seconds) {
    boost::asio::io_service ioService;
    aasdk::tcp::TCPWrapper tcpWrapper;

    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(ioService);
    std::printf("androidauto: connecting to %s:%u...\n", host.c_str(), port);
    auto ec = tcpWrapper.connect(*socket, host, port);
    if (ec) {
        std::fprintf(stderr, "androidauto: TCP connect failed: %s\n", ec.message().c_str());
        return false;
    }
    std::printf("androidauto: TCP connected, starting session\n");

    auto tcpEndpoint = std::make_shared<aasdk::tcp::TCPEndpoint>(tcpWrapper, socket);
    auto transport = std::make_shared<aasdk::transport::TCPTransport>(ioService, std::move(tcpEndpoint));
    auto session = std::make_shared<Session>(ioService);
    session->start(std::move(transport));

    // Same bounded-window approach as run_usb_probe -- this is a
    // scoped probe, not the real always-on service loop.
    boost::asio::steady_timer stopTimer(ioService, std::chrono::seconds(seconds));
    stopTimer.async_wait([&ioService](const boost::system::error_code &) { ioService.stop(); });

    ioService.run();
    return true;
}

}  // namespace androidauto
