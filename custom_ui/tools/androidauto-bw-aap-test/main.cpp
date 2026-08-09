// Standalone test for BwAapClient -- drives the full confirmed
// wireless-AA pre-connection sequence (WIFI_VERSION_REQUEST/RESPONSE,
// WIFI_START_REQUEST, WIFI_INFO_REQUEST/RESPONSE) against blueware's
// real /dev/bw_aap socket, then keeps listening and hex-dumping
// whatever follows.
//
// Usage: androidauto-bw-aap-test <ap-ip> <ap-port> <ssid> <password> <bssid> [security-mode=8] [seconds=15]
//
// ap-ip/ap-port should be this device's own hostapd AP's address and
// the port the phone should connect to for the real aasdk session
// (see wireless_probe.h -- confirming which side actually listens
// here is still open, see bw_aap_client.h). ssid/password/bssid
// should match that same AP. security-mode defaults to 8 (the raw
// value observed in real captured stock traffic, which numerically
// maps to WPA2_ENTERPRISE in aap_protobuf's WifiSecurityMode enum --
// semantically odd for a passphrase-secured AP, but matches known-
// working captured bytes; override if that turns out wrong).
//
// UNTESTED against real hardware -- /dev/bw_aap may already be held
// open by another process (sink, MsnCoreApp) when this runs; if
// connect() succeeds but nothing sensible comes back, that's the
// first thing to check.
#include <cstdio>
#include <cstdlib>

#include "androidauto/bw_aap_client.h"

int main(int argc, char **argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s <ap-ip> <ap-port> <ssid> <password> <bssid> "
                     "[security-mode=8] [seconds=15]\n",
                     argv[0]);
        return 1;
    }
    std::string apIp = argv[1];
    std::uint16_t apPort = static_cast<std::uint16_t>(std::atoi(argv[2]));
    std::string ssid = argv[3];
    std::string password = argv[4];
    std::string bssid = argv[5];
    int securityMode = argc > 6 ? std::atoi(argv[6]) : 8;
    int seconds = argc > 7 ? std::atoi(argv[7]) : 15;

    androidauto::BwAapClient client;
    if (!client.connect()) {
        return 1;
    }
    if (!client.startHandshake(apIp, apPort)) {
        std::fprintf(stderr, "androidauto: handshake steps 1-3 failed\n");
        return 1;
    }

    if (!client.respondToInfoRequest(ssid, password, bssid, securityMode, 10)) {
        std::fprintf(stderr, "androidauto: WIFI_INFO_REQUEST/RESPONSE (steps 4-5) failed\n");
        return 1;
    }

    std::printf("androidauto: full handshake sequence sent, listening for more frames for %ds...\n",
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
