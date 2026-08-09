// Standalone test driver for androidauto::run_usb_probe -- not part of
// the main custom_ui binary. Kept separate so the base UI build stays
// fast and doesn't need aasdk/Boost/OpenSSL/libusb/Protobuf just to
// build a screen with a button on it; this exists purely to prove the
// aasdk static-link graph resolves and runs, ahead of the real
// protocol/service integration work.
#include <cstdlib>

#include "androidauto/usb_probe.h"

int main(int argc, char **argv) {
    int seconds = 10;
    if (argc > 1) {
        seconds = std::atoi(argv[1]);
    }
    return androidauto::run_usb_probe(seconds) ? 0 : 1;
}
