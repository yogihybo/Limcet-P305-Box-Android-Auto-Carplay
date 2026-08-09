// Standalone test driver for androidauto::run_wireless_probe -- same
// rationale as tools/androidauto-usb-probe-test: kept out of the main
// custom_ui binary so the base UI build stays dependency-light.
//
// Usage: androidauto-wireless-probe-test <host> [port] [seconds]
// host/port must currently be supplied manually -- there is no
// automatic discovery yet (see wireless_probe.h for why).
#include <cstdio>
#include <cstdlib>

#include "androidauto/wireless_probe.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <host> [port=5277] [seconds=30]\n", argv[0]);
        return 1;
    }
    std::string host = argv[1];
    // 5277 is the commonly-cited default AA wireless port in other
    // open-source implementations -- NOT independently confirmed
    // against this project's own traffic captures, so treat it as a
    // starting guess and pass an explicit port if it doesn't work.
    std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 5277;
    int seconds = argc > 3 ? std::atoi(argv[3]) : 30;
    return androidauto::run_wireless_probe(host, port, seconds) ? 0 : 1;
}
