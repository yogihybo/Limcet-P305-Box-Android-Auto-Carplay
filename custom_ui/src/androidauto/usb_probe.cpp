#include "androidauto/usb_probe.h"

#include <cstdio>
#include <memory>

#include <boost/asio.hpp>
#include <libusb-1.0/libusb.h>

#include <aasdk/USB/USBWrapper.hpp>
#include <aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <aasdk/USB/USBHub.hpp>

namespace androidauto {

bool run_usb_probe(int seconds) {
    libusb_context *usbContext = nullptr;
    if (libusb_init(&usbContext) != 0) {
        std::fprintf(stderr, "androidauto: libusb_init failed\n");
        return false;
    }

    boost::asio::io_service ioService;
    aasdk::usb::USBWrapper usbWrapper(usbContext);
    aasdk::usb::AccessoryModeQueryFactory queryFactory(usbWrapper, ioService);
    aasdk::usb::AccessoryModeQueryChainFactory queryChainFactory(usbWrapper, ioService, queryFactory);
    auto hub = std::make_shared<aasdk::usb::USBHub>(usbWrapper, ioService, queryChainFactory);

    auto promise = aasdk::usb::IUSBHub::Promise::defer(ioService);
    promise->then(
        [](aasdk::usb::DeviceHandle) {
            std::printf("androidauto: USB device passed AOAP accessory-mode query chain\n");
        },
        [](const aasdk::error::Error &error) {
            std::printf("androidauto: USB hub stopped: %s\n", error.what());
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

    std::printf("androidauto: watching for AOAP-capable USB devices for %ds...\n", seconds);
    ioService.run();

    libusb_exit(usbContext);
    return true;
}

}  // namespace androidauto
