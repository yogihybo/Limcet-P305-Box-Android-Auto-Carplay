#include "androidauto/usb_probe.h"
#include "androidauto/log_timing.h"

#include <cstdio>
#include <memory>

#include <boost/asio.hpp>
#include <libusb-1.0/libusb.h>

#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/USBHub.hpp>
#include <aasdk/USB/AOAPDevice.hpp>
#include <aasdk/Transport/USBTransport.hpp>

#include "androidauto/session.h"

namespace androidauto {

bool run_usb_probe(int seconds) {
    libusb_context *usbContext = nullptr;
    if (libusb_init(&usbContext) != 0) {
        std::fprintf(stderr, "%s androidauto: libusb_init failed\n", androidauto::logTimestamp().c_str());
        return false;
    }

    boost::asio::io_service ioService;
    aasdk::usb::USBWrapper usbWrapper(usbContext);
    aasdk::usb::AccessoryModeQueryFactory queryFactory(usbWrapper, ioService);
    aasdk::usb::AccessoryModeQueryChainFactory queryChainFactory(usbWrapper, ioService, queryFactory);
    auto hub = std::make_shared<aasdk::usb::USBHub>(usbWrapper, ioService, queryChainFactory);

    auto promise = aasdk::usb::IUSBHub::Promise::defer(ioService);
    promise->then(
        [&usbWrapper, &ioService](aasdk::usb::DeviceHandle deviceHandle) {
            std::printf("%s androidauto: USB device passed AOAP accessory-mode query chain, "
                        "starting session\n", androidauto::logTimestamp().c_str());
            auto aoapDevice = aasdk::usb::AOAPDevice::create(usbWrapper, ioService, std::move(deviceHandle));
            auto transport = std::make_shared<aasdk::transport::USBTransport>(ioService, std::move(aoapDevice));
            auto session = std::make_shared<Session>(ioService);
            session->start(std::move(transport));
        },
        [](const aasdk::error::Error &error) {
            std::printf("%s androidauto: USB hub stopped: %s\n", androidauto::logTimestamp().c_str(), error.what());
        });
    hub->start(promise);

    // aasdk's own io_service::strand-based components need work
    // posted to actually run -- keep the io_service alive for a fixed
    // window rather than looping forever, since this is a bounded probe,
    // not the real always-on service loop.
    boost::asio::steady_timer stopTimer(ioService, std::chrono::seconds(seconds));
    stopTimer.async_wait([&hub, &ioService](const boost::system::error_code &) {
        hub->cancel();
        ioService.stop();
    });

    std::printf("%s androidauto: watching for AOAP-capable USB devices for %ds...\n", androidauto::logTimestamp().c_str(), seconds);
    ioService.run();

    libusb_exit(usbContext);
    return true;
}

}  // namespace androidauto
