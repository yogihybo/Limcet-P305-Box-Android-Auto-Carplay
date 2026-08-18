# Handoff: Android Auto Session Keep-Alive, Heartbeat & Disconnect Investigation

## 1. Executive Summary

During wireless Android Auto sessions in `custom_ui`, sessions occasionally terminate unexpectedly 20–30 seconds after connection or following an input/audio event with the following signature:
```text
[  156.444599] androidauto: control channel error: AASDK Error: 33, Native Code: 2, Additional Information:
[  156.444766] androidauto: wireless session: Session ended (io_service stopped)
```

- **`AASDK Error 33`**: Maps to `aasdk::error::ErrorCode::TCP_TRANSFER` ([`ErrorCode.hpp:60`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/third_party/aasdk/include/aasdk/Error/ErrorCode.hpp#L60)).
- **`Native Code 2`**: Maps to `boost::asio::error::eof` (Value `2`), which is Boost.Asio's standard indicator that the remote peer (the phone) closed the TCP connection.

This document details the root causes behind delayed session teardowns, compares the implementation with stock MSN (`sink` / `libAndroidAuto.so`), and provides concrete remediation steps.

---

## 2. Root Cause Analysis

### 2.1 Premature Bluetooth RFCOMM Link Closure (`/dev/bw_aap`)
- **Location**: [`custom_ui/src/androidauto/wireless_session_manager.cpp:325-327`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L325-L327)
- **Mechanism**:
  - `custom_ui` sends `WIFI_INFO_RESPONSE`, waits up to 15 seconds for an optional `WIFI_CONNECT_STATUS`, and then calls `bwAap.close()` immediately before entering `acceptor.accept()`.
  - Closing `/dev/bw_aap` causes the Feasycom `blueware` daemon to terminate the active Bluetooth RFCOMM channel with the phone.
  - On many modern Android devices, Gearhead's `WirelessConnectionManager` uses the Bluetooth RFCOMM link as an **out-of-band watchdog/tether**. When RFCOMM drops, the phone initiates a 20–30 second watchdog timer. If RFCOMM is not restored, the phone assumes the vehicle's ignition turned off and tears down the WiFi TCP link.
- **Stock MSN Behavior**:
  - Disassembly of stock `sink` (`libAndroidAuto.so`, `RfcommConnectionPrivate::run()`) shows the vendor app **keeps `/dev/bw_aap` open continuously** across the entire lifetime of the session, servicing incoming status frames in a background thread.

### 2.2 Missing `TCP_NODELAY` and `SO_KEEPALIVE` on Accepted Socket
- **Location**: [`custom_ui/src/androidauto/wireless_session_manager.cpp:347-366`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L347-L366)
- **Mechanism**:
  - When the incoming TCP connection is accepted, `custom_ui` creates a `TCPEndpoint` without setting socket options:
    - Without `TCP_NODELAY`: Nagle's algorithm buffers small outgoing packets (`PingResponse`, `InputReport`, `MediaAckIndication`), waiting for full MSS packets or delayed ACKs (adding up to 200ms of latency per control frame).
    - Without `SO_KEEPALIVE`: The kernel does not detect silent WiFi half-open states or transmit TCP-level keepalive probes.

### 2.3 Overly Tight Ping Timeout Thresholds (`pingConfig`)
- **Location**: [`custom_ui/src/androidauto/session.cpp:282-287`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp#L282-L287)
- **Mechanism**:
  - `custom_ui` advertises `pingConfig->set_timeout_ms(3000)` (3-second timeout) and `high_latency_threshold_ms(200)`.
  - On an ARM Cortex-A5 single-core processor, concurrent 30fps H.264 video decoding, UI rendering, and ALSA writes share the single `io_service` thread. Heavy video I-frame bursts can delay processing of `onPingRequest` past the 3000ms window, triggering a phone-side keepalive timeout.
  - Standard reference implementations (OpenAuto, OpenAutoLink) advertise `timeout_ms = 10000` (10s) and `interval_ms = 2000` to absorb embedded CPU scheduling spikes.

### 2.4 Stop-and-Wait Audio Flow Control Backpressure
- **Location**: [`custom_ui/src/androidauto/audio_channel.cpp:41-43, 88`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp#L41-L43)
- **Mechanism**:
  - When `max_unacked = 1` and ACKs are gated behind blocking ALSA playback, any audio stall starves the phone's media pipeline.
  - If media credits are not replenished for prolonged periods (>15–20 seconds), Gearhead's internal audio watchdog times out and terminates the session.

---

## 3. Stock MSN (`sink`) Architecture Comparison

| Subsystem | `custom_ui` Current | Stock MSN (`sink` / `libAndroidAuto.so`) |
| :--- | :--- | :--- |
| **Bluetooth RFCOMM (`/dev/bw_aap`)** | Closed after 15s handshake wait | Kept open and continuously polled in background |
| **Socket Options** | Default blocking socket (Nagle ON) | `TCP_NODELAY` enabled |
| **Ping Timeout (`pingConfig`)** | `3000ms` | `10000ms` (10 seconds) |
| **Audio ACKs** | Synchronously gated behind ALSA write | Asynchronously acknowledged on ingest (`WorkQueue`) |

---

## 4. Remediation Plan & Source Diffs

### 4.1 Enable `TCP_NODELAY` and `SO_KEEPALIVE`
In [`custom_ui/src/androidauto/wireless_session_manager.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L347-L364):

```diff
--- a/custom_ui/src/androidauto/wireless_session_manager.cpp
+++ b/custom_ui/src/androidauto/wireless_session_manager.cpp
@@ -350,6 +350,11 @@ void WirelessSessionManager::run() {
     if (acceptEc) {
         setStatus(WirelessSessionState::Failed, "accept() failed: " + acceptEc.message());
         return;
     }
+
+    // Disable Nagle's algorithm for low-latency ping/ack delivery and enable TCP keepalives
+    socket->set_option(boost::asio::ip::tcp::no_delay(true));
+    socket->set_option(boost::asio::socket_base::keep_alive(true));
+
     boost::system::error_code peerEc;
     auto remote = socket->remote_endpoint(peerEc);
```

### 4.2 Increase Ping Timeout Thresholds
In [`custom_ui/src/androidauto/session.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/session.cpp#L282-L287):

```diff
--- a/custom_ui/src/androidauto/session.cpp
+++ b/custom_ui/src/androidauto/session.cpp
@@ -280,7 +280,7 @@ void Session::onServiceDiscoveryRequest(
     // independently tuned for this hardware.
     auto *pingConfig = response.mutable_connection_configuration()->mutable_ping_configuration();
-    pingConfig->set_tracked_ping_count(5);
-    pingConfig->set_timeout_ms(3000);
-    pingConfig->set_interval_ms(1000);
-    pingConfig->set_high_latency_threshold_ms(200);
+    pingConfig->set_tracked_ping_count(10);
+    pingConfig->set_timeout_ms(10000);
+    pingConfig->set_interval_ms(2000);
+    pingConfig->set_high_latency_threshold_ms(1000);
```

### 4.3 Keep `/dev/bw_aap` Open During the Session
In [`custom_ui/src/androidauto/wireless_session_manager.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L325-L327):

```diff
--- a/custom_ui/src/androidauto/wireless_session_manager.cpp
+++ b/custom_ui/src/androidauto/wireless_session_manager.cpp
@@ -324,5 +324,4 @@ void WirelessSessionManager::run() {
     // call now actually waits through (see its own updated comment).
     bwAap.waitForOptionalConnectStatus(15);
-    std::printf("%s androidauto: wireless session: closing bw_aap\n", androidauto::logTimestamp().c_str());
-    bwAap.close();
 
     setStatus(WirelessSessionState::Connecting,
@@ -382,4 +381,5 @@ void WirelessSessionManager::run() {
 
     {
         std::lock_guard<std::mutex> lock(sessionMutex_);
         currentSession_.reset();
     }
+    bwAap.close();
```

---

## 5. Verification & Testing Checklist

1. **Keep-Alive Stability**:
   - Establish wireless Android Auto connection.
   - Leave the vehicle stationary on the home/navigation screen with no audio playing.
   - Verify that the connection remains active past 5 minutes with zero unexpected `EOF` drops.
2. **Ping Latency & Delivery**:
   - Monitor control channel packet delivery during concurrent 30fps H.264 video decoding.
   - Confirm ping/pong round trips succeed within the 10s window without tripping error handlers.
3. **Bluetooth Tether Liveness**:
   - Verify that keeping `/dev/bw_aap` open preserves the phone's Bluetooth wireless watchdog connection throughout active navigation.
