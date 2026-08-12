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

    // Drives steps 1-3: sends WIFI_VERSION_REQUEST, waits for
    // WIFI_VERSION_RESPONSE (logged raw, not parsed), then sends
    // WIFI_START_REQUEST with the given AP connect target.
    //
    // 2026-08-12: also now waits (bounded, a few seconds) for the
    // WIFI_START_RESPONSE (type 7) that can follow -- its proto
    // (optional ip_address, optional port, required status) can carry
    // the PHONE's own authoritative connect-back address/port,
    // overriding what we proposed. Real hardware evidence this
    // matters: a session where the phone's own AA app showed
    // "connected" while this device's TCP *listener* (a same-day, now-
    // reverted attempt at making the head unit the server) never saw
    // an incoming connection at all -- strong evidence the phone
    // expects US to connect to IT, on a port we can't just assume is
    // apPort. If the response arrives and carries a nonempty
    // ip_address/nonzero port, `outIp`/`outPort` are set to those;
    // otherwise they're left untouched (caller should keep its own
    // fallback in that case). Returns true even if no
    // WIFI_START_RESPONSE arrives at all within the timeout -- this
    // project's own real packet capture (docs/logs) only confirmed
    // steps 1-5, not a type-7 reply, so its absence isn't treated as a
    // handshake failure, just "nothing to override with."
    bool startHandshake(const std::string &apIpAddress, std::uint16_t apPort, std::string &outIp,
                         std::uint16_t &outPort);

    // Waits (bounded by timeoutSeconds) for the phone's
    // WIFI_INFO_REQUEST (type 2, step 4), then sends WIFI_INFO_RESPONSE
    // (type 3, step 5) with the given AP credentials. securityMode uses
    // aap_protobuf::service::wifiprojection::message::WifiSecurityMode's
    // numeric values -- the real captured traffic used the raw value 8
    // (WPA2_ENTERPRISE per that enum), passed through as-is by default
    // to match known-good captured bytes rather than guess.
    //
    // CROSS-CHECKED against firmware_source/mtd6_rootfs/etc/hostapd/
    // hostapd.conf (the real static template, `wpa_passphrase=88888888`
    // -- matches the captured password exactly) and found a genuine
    // discrepancy worth flagging, not just a "seems odd" hunch: that
    // config uses `wpa=2`/`wpa_key_mgmt=WPA-PSK` -- ordinary WPA2
    // *personal*, not enterprise. So either (a) the vendored
    // WifiSecurityMode enum's numbering doesn't match what this
    // firmware actually transmits (same class of issue as
    // WifiVersionRequest/Response), or (b) this field isn't load-
    // bearing and phones ignore it in practice. If value 8 doesn't
    // work when hardware-tested, try WPA2_PERSONAL (5) next -- that's
    // what the real AP config actually is.
    //
    // Also cross-checked: the real SSID observed was "carplay_fc9f",
    // not the static template's "carplay_wifi" -- MsnCoreApp rewrites
    // the SSID at runtime with a device-specific suffix matching the
    // last 4 hex chars of the Bluetooth MAC (blueware's own captured
    // log showed the same "fc9f" suffix on its broadcast name,
    // "Limcet Box_fc9f", for BD_ADDR DC0D3014FC9F) -- the password
    // appears to stay fixed at "88888888" across instances. Not
    // reverse-engineered further (closed binary); pass whatever the
    // real live AP's actual SSID/password are once that's wired up.
    //
    // Returns false if WIFI_INFO_REQUEST doesn't arrive within the
    // timeout, or on send failure.
    bool respondToInfoRequest(const std::string &ssid, const std::string &password,
                               const std::string &bssid, int securityMode, int timeoutSeconds);

    // Returns false on read error/timeout. On success, fills type and
    // payload with what was received. Public so a test driver can keep
    // observing frames past what startHandshake() itself handles (e.g.
    // the WIFI_INFO_REQUEST/WIFI_INFO_RESPONSE exchange -- steps 4-5 in
    // the header comment above -- not yet driven by this class).
    //
    // 2026-08-12: checks a one-frame pending buffer first (see
    // pushBackFrame()) before touching the socket at all -- see that
    // function's comment for why this exists: a real hardware
    // regression where startHandshake()'s WIFI_START_RESPONSE wait
    // read the phone's WIFI_INFO_REQUEST by mistake (this vendor stack
    // apparently never sends a type-7 reply) and discarded it, causing
    // respondToInfoRequest()'s own wait to time out on a frame that had
    // already arrived and been thrown away.
    bool receiveFrame(std::uint16_t &type, std::string &payload, int timeoutSeconds);

private:
    bool sendFrame(std::uint16_t type, const std::string &payload);

    // Stashes a frame this class read speculatively (e.g. while
    // startHandshake() is only hoping for a WIFI_START_RESPONSE) but
    // turned out to be something else -- the next receiveFrame() call
    // returns it instead of reading a fresh one from the socket, so no
    // frame is ever silently lost to a wrong guess about what's coming
    // next. At most one frame is ever buffered (this protocol is
    // strictly request/response, never concurrent streams).
    void pushBackFrame(std::uint16_t type, std::string payload);

    int fd_ = -1;
    bool hasPendingFrame_ = false;
    std::uint16_t pendingType_ = 0;
    std::string pendingPayload_;
};

}  // namespace androidauto
