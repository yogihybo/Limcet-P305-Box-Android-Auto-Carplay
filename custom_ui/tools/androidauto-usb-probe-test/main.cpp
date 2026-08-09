// Standalone test driver for androidauto::run_usb_probe -- not part of
// the main custom_ui binary. Kept separate so the base UI build stays
// fast and doesn't need aasdk/Boost/OpenSSL/libusb/Protobuf just to
// build a screen with a button on it.
//
// Now drives a real androidauto::Session (version request + SSL
// handshake) once a device passes AOAP detection, not just USB-level
// detection -- default window widened accordingly to cover handshake
// round trips, override with an explicit argument if needed.
#include <cstdlib>

#include "androidauto/usb_probe.h"

int main(int argc, char **argv) {
    int seconds = 30;
    if (argc > 1) {
        seconds = std::atoi(argv[1]);
    }
    return androidauto::run_usb_probe(seconds) ? 0 : 1;
}
