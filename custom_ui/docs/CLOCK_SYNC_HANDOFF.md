# Clock Synchronization Engineering Handoff

## 1. Problem Statement & Architecture Realities

* **Hardware Limitation**: The ARK1680 (Limcet P305/P306) board has no dedicated real-time clock (RTC) backup battery. On every cold boot / power cycle, the Linux system clock (`CLOCK_REALTIME`) resets to UNIX epoch (`1970-01-01 00:00:00 UTC`).
* **UI Requirement**: The persistent status bar digital clock widget (`lv_font_roboto_28`) requires accurate wall-clock time without manual configuration.

### Protocol Directionality & Android Auto Constraints:
1. **`SensorSource` Directionality**: In the Android Auto specification, `SensorSource` is a service hosted by the **Head Unit** to feed vehicle sensors (Driving Status, Night Mode, Wheel Speed, GPS) *into the phone*. The phone is the *sink* (consumer) and never transmits sensor or location data back to the head unit.
2. **AAP Media Streams & Keep-Alives**:
   * `PingRequest.timestamp`: Uses `SystemClock.elapsedRealtime()` / `System.nanoTime()` (monotonic microseconds since phone boot), not absolute wall-clock epoch.
   * `MediaWithTimestampIndication`: Audio/video frame timestamps are relative presentation timestamps (PTS) for AV synchronization, not real-time clock values.
3. **Network Topology (WPP)**: The head unit operates as the WiFi SoftAP (`192.168.43.1`) and TCP server, while the phone is a station client (`192.168.43.x`). Android client firewalls block inbound network probes (SNTP/HTTP).

**Conclusion**: The core Android Auto Protocol (AAP) does **not** transmit real wall-clock time from phone to head unit.

---

## 2. The Verified Solution: Bluetooth PBAP / Call History Timestamps

Because the phone pairs and connects to the head unit over classic Bluetooth via Feasycom's `blueware` daemon on `/dev/bw_serial` prior to projection, the phone-to-head-unit Bluetooth profile channel provides the exact phone clock.

```
[Phone (Bluetooth Peer)] <============ /dev/bw_serial ============> [blueware Daemon] <==> [custom_ui]
           |                                                                 |                 |
           | <--- AT+PBDOWN=1 (Query Recent Call History via PBAP) --------- | <--- send_command
           | ---> +PBENTRY=... 20260818T160600 (Call Record Timestamp) ----> | ---> response
           |                                                                 |                 |
                                                                                       clock_settime()
```

### Technical Implementation Details:
1. **Trigger**: When Bluetooth connection is established (`+DEVSTAT=3` or `+HFPSTAT=3` observed on `hal::shared_handle()`).
2. **Command Dispatch**:
   * Send `AT+PBDOWN=1` via `hal::send_command(h, "PBDOWN=1", "+PBENTRY=", out_lines, 2000)`.
   * The phone returns its most recent call history record with an ISO-8601 timestamp string:
     ```
     +PBENTRY=1,"1234567890",129,"Name","20260818T160600"
     ```
3. **Clock Extraction & System Time Calibration**:
   * Parse the timestamp string (`YYYYMMDDTHHMMSS` -> `Year: 2026, Month: 08, Day: 18, Hour: 16, Min: 06, Sec: 00`).
   * Convert to `time_t` epoch and set the Linux system clock:
     ```cpp
     struct tm tm_time = {};
     // parse 20260818T160600 into tm_time
     time_t epoch = mktime(&tm_time);
     struct timespec ts = { epoch, 0 };
     clock_settime(CLOCK_REALTIME, &ts);
     ```
4. **UI Auto-Update**:
   * `custom_ui/src/ui/staging/home_dashboard.cpp` polls `std::time(nullptr)` once per second.
   * As soon as `clock_settime(CLOCK_REALTIME, ...)` executes, all top-bar clock labels automatically display local time.

---

## 3. Alternative Fallback: NMEA GPS Hardware Stream
If an external vehicle GPS antenna is wired to the MCU/UART:
* The NMEA `$GPRMC` sentence outputs UTC date and time once per second:
  ```
  $GPRMC,160618.00,A,3745.1234,N,12225.5678,W,0.0,0.0,180826,,,A*7C
  ```
* Parsing `160618.00` (16:06:18 UTC) and `180826` (18 Aug 2026) directly updates `CLOCK_REALTIME`.
