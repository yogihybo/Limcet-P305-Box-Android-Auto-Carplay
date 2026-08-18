# Clock Synchronization Engineering Handoff

## 1. Problem Statement & Hardware Context

* **Hardware Limitation**: The ARK1680 (Limcet P305/P306) board has no dedicated real-time clock (RTC) backup battery. On every cold boot / power cycle, the Linux system clock (`CLOCK_REALTIME`) resets to UNIX epoch (`1970-01-01 00:00:00 UTC`).
* **UI Impact**: The persistent status bar and digital clock widget (`lv_font_roboto_28`) require accurate local wall-clock time.
* **Network Topology Note**: In Wireless Android Auto (WPP), the **Head Unit operates as the WiFi SoftAP / Gateway (`192.168.43.1`)** and TCP Server, while the Phone connects as a WiFi Station/Client (`192.168.43.x`). Inbound network probing (SNTP/HTTP) to the phone's client IP is blocked by Android's client-side firewall.
* **3GPP AT+CCLK Failure**: Android's phone-side Bluetooth HFP stack does not implement `AT+CCLK` / `AT+CCLK?` (confirmed by inspecting AOSP `bta_ag_cmd.cc`; Android returns `ERR004`).

---

## 2. Verified Synchronization Strategies

### Strategy 1: Bluetooth PBAP Call Record Timestamp (Recommended)

#### Architecture & Flow
Before Android Auto WiFi starts, the phone pairs and connects to the head unit via classic Bluetooth (HFP/A2DP) managed by Feasycom's `blueware` daemon on `/dev/bw_serial`.

```
[Phone (Bluetooth Peer)] <============ /dev/bw_serial ============> [blueware Daemon] <==> [custom_ui]
           |                                                                 |                 |
           | <--- AT+PBDOWN=1 (Query Recent Call History via PBAP) --------- | <--- send_command
           | ---> +PBENTRY=... 20260818T155600 (Call Record Timestamp) ----> | ---> response
           |                                                                 |                 |
                                                                                       clock_settime()
```

#### Technical Implementation
1. **Trigger**: On first Bluetooth connection (`+DEVSTAT=3` or `+HFPSTAT=3` observed on `hal::shared_handle()`).
2. **Command Dispatch**:
   * Send `AT+PBDOWN=1` via `hal::send_command(h, "PBDOWN=1", "+PBENTRY=", out_lines, 2000)`.
   * The response provides the most recent call record with ISO-8601 timestamp:
     ```
     +PBENTRY=1,"1234567890",129,"Name","20260818T155600"
     ```
   * Extract timestamp string `20260818T155600`, parse year/month/day/hour/min/sec, and call `clock_settime(CLOCK_REALTIME, &ts)`.
3. **Pros & Cons**:
   * **Pros**: Independent of WiFi AP topology; works immediately on Bluetooth connection before projection starts; supported by both Android and iOS.
   * **Cons**: Requires PBAP access permission granted during initial pairing.

---

### Strategy 2: Android Auto Sensor Channel (`LocationData.timestamp_ms`)

#### Architecture & Flow
Within an active Android Auto session, the phone streams location and GPS telemetry to the head unit over the Sensor Channel (`SensorSourceService`).

```
[Phone (Android Auto / Google Maps)] ============= AAP TCP Session =============> [Head Unit]
                  |                                                                     |
                  | --- SensorIndication (LocationData with timestamp_ms) ------------> |
                  |                                                                     |
                                                                          clock_settime(CLOCK_REALTIME, &ts)
```

#### Technical Implementation
1. **Message Definition** (`LocationData.proto`):
   ```protobuf
   message LocationData {
       optional uint64 timestamp_ms = 8; // Genuine UTC epoch in milliseconds
       optional int32 latitude = 2;
       optional int32 longitude = 3;
       optional int32 speed = 5;
       optional int32 bearing = 6;
   }
   ```
2. **Trigger**: In `SensorChannel::onLocationDataIndication(const aap_protobuf::service::sensorsource::message::LocationData & data)`:
   * When `data.has_timestamp_ms()`, convert milliseconds to seconds/nanoseconds:
     ```cpp
     struct timespec ts;
     ts.tv_sec = data.timestamp_ms() / 1000;
     ts.tv_nsec = (data.timestamp_ms() % 1000) * 1000000;
     clock_settime(CLOCK_REALTIME, &ts);
     ```
3. **Pros & Cons**:
   * **Pros**: Continuous millisecond-accurate GPS time synchronization directly over the established AAP protocol channel.
   * **Cons**: Only active when a navigation/location service is running on the phone.

---

## 3. Recommended Implementation Plan

1. **Boot / Bluetooth Connection**: Issue `AT+PBDOWN=1` via PBAP over `/dev/bw_serial` to seed the Linux wall-clock immediately upon Bluetooth link establishment.
2. **Android Auto Active Projection**: Parse `LocationData.timestamp_ms` in `SensorChannel` to continuously calibrate the system time to GPS/network precision.
3. **UI Integration**: `custom_ui/src/ui/staging/home_dashboard.cpp` polls `std::time(nullptr)` once per second; once `CLOCK_REALTIME` is calibrated, all UI clock widgets (`lv_font_roboto_28`) automatically display accurate local time.
