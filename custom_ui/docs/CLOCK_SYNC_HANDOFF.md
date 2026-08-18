# Clock Synchronization Engineering Handoff

## 1. Problem Statement & Hardware Context

* **Hardware Limitation**: The ARK1680 (Limcet P305/P306) board has no dedicated real-time clock (RTC) backup battery. On every cold boot / power cycle, the Linux system clock (`CLOCK_REALTIME`) resets to UNIX epoch (`1970-01-01 00:00:00 UTC`) or the kernel build timestamp.
* **UI Impact**: The persistent status bar and digital clock widget displayed across the Home Dashboard, Bluetooth Screen, Settings Screen, and Navigation Rail require an accurate wall-clock time without forcing manual user configuration.
* **Standard 3GPP AT+CCLK Failure**: Android's phone-side Bluetooth HFP stack does not implement `AT+CCLK` / `AT+CCLK?` (confirmed by inspecting AOSP `bta_ag_cmd.cc` in Android 10 through Android 15; Android returns `ERR004`).

---

## 2. Proposed Synchronization Strategies

### Option 1: Android Auto Local WiFi Gateway Time Probe (Recommended)

#### Architecture & Flow
When wireless Android Auto establishes its session, the head unit and phone share a direct TCP/IP subnet (typically `192.168.43.0/24` with the phone as gateway `192.168.43.1`, or vice-versa when the head unit operates as AP).

```
[Phone (192.168.43.1)] <================ TCP/IP Link ================> [Head Unit (192.168.43.x)]
         |                                                                      |
         | <--- Non-blocking HTTP HEAD / SNTP probe on link up ---------------- |
         | ---> HTTP 200/404 with "Date: Tue, 18 Aug 2026 04:45:39 GMT" ------> |
         |                                                                      |
                                                                  clock_settime(CLOCK_REALTIME, &ts)
                                                                  LVGL Clock Widget Auto-Refreshes
```

#### Technical Implementation
1. **Trigger**: Listen to `hal::AndroidAutoClient::watch_broadcasts()` or sidecar state change transitioning to `STATE Connected`.
2. **Probe**:
   * Open a non-blocking TCP socket to port 80/8080 or the AASDK port on the phone's gateway IP.
   * Send a minimal HTTP request:
     ```http
     HEAD / HTTP/1.1\r\nHost: 192.168.43.1\r\nConnection: close\r\n\r\n
     ```
   * Parse the returned RFC-2822 `Date:` header:
     ```http
     HTTP/1.1 200 OK
     Date: Tue, 18 Aug 2026 04:45:39 GMT
     Server: Android
     ```
   * Convert RFC-2822 date string via `strptime("%a, %d %b %Y %H:%M:%S GMT")` and `timegm()`.
   * Set Linux system time:
     ```cpp
     struct timespec ts;
     ts.tv_sec = parsed_epoch;
     ts.tv_nsec = 0;
     clock_settime(CLOCK_REALTIME, &ts);
     ```
3. **Pros & Cons**:
   * **Pros**: Sub-second precision; zero Bluetooth AT-command contention; 100% compliant across all Android and iOS devices.
   * **Cons**: Operates only after Android Auto WiFi session has associated.

---

### Option 2: Bluetooth PBAP / Call Log Timestamp Parsing

#### Architecture & Flow
Before WiFi or Android Auto starts, the phone connects to the head unit via classic Bluetooth HFP/A2DP managed by Feasycom's `blueware` daemon on `/dev/bw_serial`.

```
[Phone (BT Audio Gateway)] <============ /dev/bw_serial ============> [blueware Daemon] <==> [custom_ui]
           |                                                                 |                 |
           | <--- AT+PBDOWN=1 (Query Recent Call History via PBAP) --------- | <--- send_command
           | ---> +PBENTRY=... 20260818T144500 (Call Record Timestamp) ----> | ---> response
           |                                                                 |                 |
                                                                                       clock_settime()
```

#### Technical Implementation
1. **Trigger**: Triggered on first Bluetooth HFP/A2DP connection (`+DEVSTAT=3` or `+HFPSTAT=3` seen on `hal::shared_handle()`).
2. **Command Dispatch**:
   * Send `AT+PBDOWN=1` via `hal::send_command(h, "PBDOWN=1", "+PBENTRY=", out_lines, 2000)`.
   * The response provides the most recent call record with ISO-8601 timestamp:
     ```
     +PBENTRY=1,"1234567890",129,"Name","20260818T144500"
     ```
   * Extract timestamp string `20260818T144500`, parse year/month/day/hour/min/sec, and call `clock_settime(CLOCK_REALTIME, &ts)`.
3. **Pros & Cons**:
   * **Pros**: Sets system clock immediately upon Bluetooth connection before the driver even launches Android Auto projection.
   * **Cons**: Requires PBAP access permission granted on the phone; requires one AT-command transaction over `/dev/bw_serial`.

---

## 3. Recommended Hybrid Implementation Plan

1. **Phase 1 (BT Link)**: When Bluetooth connects, issue `AT+PBDOWN=1` to seed coarse wall-clock time so the UI immediately shows the correct hour/minute.
2. **Phase 2 (WiFi Link)**: When wireless Android Auto starts, query the gateway's HTTP `Date` header to achieve high-precision synchronization.
3. **UI Integration**: `custom_ui/src/ui/staging/home_dashboard.cpp` already polls `std::time(nullptr)` each second; once `clock_settime()` updates `CLOCK_REALTIME`, all LVGL clock labels automatically display local time.
