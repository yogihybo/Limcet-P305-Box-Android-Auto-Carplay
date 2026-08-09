#pragma once

#include <cstdint>
#include <string>

namespace androidauto {

// Client for blueware's (Feasycom BT stack, /usr/bin/blueware) local
// Unix domain socket at /dev/bw_aap -- the REAL Bluetooth
// pre-connection channel this device uses for wireless Android
// Auto/CarPlay, confirmed from real captured traffic in
// docs/logs/android auto log v{1,2,3}.txt (stock `sink` binary,
// class name `BtRfcommController`). This supersedes the earlier
// BluetoothRFCOMMTransport/accept_rfcomm_connection approach (raw
// AF_BLUETOOTH/BTPROTO_RFCOMM kernel socket + our own SDP server) --
// that assumed a standard Linux BlueZ stack, which this device
// doesn't have. blueware already owns the adapter, already handles
// pairing/RFCOMM/SDP internally, and already exposes a clean local
// socket for exactly this purpose -- we just need to be a client of
// it, the same pattern as `sink`'s own D-Bus interface
// (com.arkmicro.auto, see ARCHITECTURE.md).
//
// Wire format (confirmed from the captured logs, not guessed):
//   [uint16 length, big-endian][uint16 type, big-endian][protobuf payload]
// where `type` corresponds to aap_protobuf::aaw::MessageId (a small,
// separate protobuf schema aasdk vendors specifically for this
// pre-connection exchange -- WIFI_START_REQUEST=1, WIFI_INFO_REQUEST=2,
// WIFI_INFO_RESPONSE=3, WIFI_VERSION_REQUEST=4, WIFI_VERSION_RESPONSE=5).
// Confirmed message sequence from the real capture:
//   1. HU -> phone: WIFI_VERSION_REQUEST (type 4) -- sent as the exact
//      observed byte sequence, NOT constructed from aasdk's own
//      WifiVersionRequest.proto (that proto is empty -- `message
//      WifiVersionRequest {}` -- but the real captured frame has a
//      9-byte non-empty payload, so the vendored proto is evidently
//      incomplete/wrong for this message; replaying known-good bytes
//      is safer than trusting it)
//   2. phone -> HU: WIFI_VERSION_RESPONSE (type 5), containing (per the
//      log's own field dump) majorVer/minorVer/deviceSerial/status/
//      channel -- also doesn't cleanly match aasdk's WifiVersionResponse
//      proto (4 anonymous "unknown_value_*" fields), so this class only
//      logs the raw bytes, doesn't attempt to parse it
//   3. HU -> phone: WIFI_START_REQUEST (type 1) -- THIS message's proto
//      (`ip_address` string, `port` uint32) is clean and unambiguous,
//      so this class constructs it for real via aasdk's generated
//      aap_protobuf::aaw::WifiStartRequest class, filling in our own
//      hostapd AP's IP and the TCP port our own Session will listen^H^H
//      ^H^H^H connect from (this device's own wired-AA-over-TCP
//      "Session" always connects OUT, per prior research -- so this
//      is actually the phone's own connect target for OUR AP, not a
//      port we need to listen on ourselves; needs confirming against
//      a live test)
//   4. phone -> HU: WIFI_INFO_REQUEST (type 2, empty -- matches its
//      proto, which genuinely has no fields)
//   5. HU -> phone: WIFI_INFO_RESPONSE (type 3) -- also a clean proto
//      match (`ssid`/`password`/`bssid`/`security_mode`), confirmed
//      field-for-field against the real capture bytes. Constructed via
//      aasdk's generated WifiInfoResponse class with our own AP's
//      SSID/password/BSSID.
//
// NOT hardware-tested by this project yet -- this is a faithful
// reconstruction of real captured stock traffic, not confirmed to work
// when driven by our own code against a real phone. Also unconfirmed:
// whether /dev/bw_aap accepts more than one simultaneous client
// connection (if `sink` or MsnCoreApp already holds it open when our
// code tries to connect, this may fail or interfere -- test carefully,
// don't assume).
class BwAapClient {
public:
    BwAapClient();
    ~BwAapClient();

    // Opens /dev/bw_aap. Returns false on failure (logs the reason).
    bool connect();

    void close();

    // Drives the confirmed message sequence: sends WIFI_VERSION_REQUEST,
    // waits for WIFI_VERSION_RESPONSE (logged raw, not parsed), then
    // sends WIFI_START_REQUEST with the given AP connect target. This
    // is as far as this class goes for now -- handling the
    // WIFI_INFO_REQUEST/WIFI_INFO_RESPONSE exchange that follows (steps
    // 4-5 above) is the next increment, once step 1-3 is confirmed
    // against a real phone.
    bool startHandshake(const std::string &apIpAddress, std::uint16_t apPort);

    // Returns false on read error/timeout. On success, fills type and
    // payload with what was received. Public so a test driver can keep
    // observing frames past what startHandshake() itself handles (e.g.
    // the WIFI_INFO_REQUEST/WIFI_INFO_RESPONSE exchange -- steps 4-5 in
    // the header comment above -- not yet driven by this class).
    bool receiveFrame(std::uint16_t &type, std::string &payload, int timeoutSeconds);

private:
    bool sendFrame(std::uint16_t type, const std::string &payload);

    int fd_ = -1;
};

}  // namespace androidauto
