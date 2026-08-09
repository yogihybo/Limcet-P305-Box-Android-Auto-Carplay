// Standalone test for BwAapClient -- drives the confirmed
// WIFI_VERSION_REQUEST/RESPONSE + WIFI_START_REQUEST exchange against
// blueware's real /dev/bw_aap socket, then keeps listening and
// hex-dumping whatever frames follow (WIFI_INFO_REQUEST is expected
// next per the captured logs, but this tool doesn't respond to it --
// that's the next increment once this step is confirmed working).
//
// Usage: androidauto-bw-aap-test <ap-ip> <ap-port> [seconds=15]
//
// ap-ip/ap-port should be this device's own hostapd AP's address and
// the port our own wireless Session (see wireless_probe.h) would
// listen^H^H^H^H connect from -- see bw_aap_client.h's header comment
// for the open question about which side actually listens here.
//
// UNTESTED against real hardware -- /dev/bw_aap may already be held
// open by another process (sink, MsnCoreApp) when this runs; if
// connect() succeeds but nothing sensible comes back, that's the
// first thing to check.
#include <cstdio>
#include <cstdlib>

#include "androidauto/bw_aap_client.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <ap-ip> <ap-port> [seconds=15]\n", argv[0]);
        return 1;
    }
    std::string apIp = argv[1];
    std::uint16_t apPort = static_cast<std::uint16_t>(std::atoi(argv[2]));
    int seconds = argc > 3 ? std::atoi(argv[3]) : 15;

    androidauto::BwAapClient client;
    if (!client.connect()) {
        return 1;
    }
    if (!client.startHandshake(apIp, apPort)) {
        std::fprintf(stderr, "androidauto: handshake failed\n");
        return 1;
    }

    std::printf("androidauto: handshake steps 1-3 sent, listening for more frames for %ds...\n",
                seconds);
    // No proper event loop here (this test doesn't need boost::asio) --
    // just keep calling the blocking receiveFrame with a bounded
    // per-call timeout until the overall window elapses.
    for (int elapsed = 0; elapsed < seconds;) {
        std::uint16_t type = 0;
        std::string payload;
        if (!client.receiveFrame(type, payload, 3)) {
            elapsed += 3;
            continue;
        }
        std::printf("androidauto: received type=%u, %zu bytes:", type, payload.size());
        for (unsigned char byte : payload) {
            std::printf(" %02x", static_cast<unsigned>(byte));
        }
        std::printf("\n");
    }

    return 0;
}
