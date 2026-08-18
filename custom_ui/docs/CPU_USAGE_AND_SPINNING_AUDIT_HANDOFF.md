# Handoff: CPU Usage, Thread Spinning & Execution Overhead Audit

## 1. Executive Summary

This document details an audit of `custom_ui` and its sidecars for CPU usage spikes, thread spinning, unbounded queue scaling, and busy-wait loops on the ArkMicro ARK1668 platform (single-core ARM Cortex-A5).

Four specific mechanisms were identified that degrade system performance or cause CPU utilization to scale over time:
1. **Fork/Exec Storm on Disconnected Sidecar**: Up to 4–8 process forks per second triggered by 500ms status polling when `androidauto-sidecar` is offline or crashed.
2. **Fixed 200 Hz LVGL Wakeup Loop**: Unconditional 5ms sleep in `main.cpp` causing 200 wakeups/sec even when the UI is completely static.
3. **Unbounded Queue Overhead in `aasdk::Messenger`**: Queue expansion from unthrottled video streams increasing CPU and memory allocation overhead.
4. **Non-Blocking UART Read Spin on `EAGAIN`**: Potential busy-wait in `mcu_input.cpp` during transient serial error states.

---

## 2. Detailed Findings & Root Cause Analysis

### 2.1 The Fork/Exec Storm on Disconnected Sidecar
- **Location**: [`custom_ui/src/hal/androidauto_client.cpp:54-78, 166-176`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/androidauto_client.cpp#L54-L78)
- **Mechanism**:
  - `ui/android_auto_screen.cpp` polls `client().statusLine(allow_spawn=true)` every **500ms**.
  - When the sidecar is offline or terminated by the OOM killer, `statusLine()` executes 2 connection attempts per poll.
  - Each attempt calls `ensureConnected(allow_spawn=true)` $\rightarrow$ `trySpawnSidecar()`, which runs:
    ```cpp
    std::system("pidof androidauto-sidecar >/dev/null 2>&1"); // Fork 1: /bin/sh -> pidof
    std::system(cmd.c_str());                                  // Fork 2: /bin/sh -> androidauto-sidecar
    ```
  - This executes **4 to 8 shell process forks per second on the single LVGL UI thread**.
  - On a 500–800MHz Cortex-A5 core, continuous fork/exec syscalls consume significant CPU time, cause scheduler thrashing, and freeze the UI thread.
- **Remediation**: Rate-limit sidecar spawn attempts to at most once every 5 seconds using an in-process monotonic timestamp check.

### 2.2 Static 200 Hz LVGL Main Loop Tick Rate
- **Location**: [`custom_ui/src/main.cpp:466-494`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/main.cpp#L466-L494)
- **Mechanism**:
  - The main loop runs an unconditional 5ms sleep:
    ```cpp
    while (true) {
        lv_timer_handler();
        ...
        usleep(5000); // 200 wakeups/sec unconditionally
    }
    ```
  - `lv_timer_handler()` returns the exact number of milliseconds until the next scheduled LVGL timer is due (often 30–100ms when static).
  - Waking 200 times per second on a static screen prevents the CPU from entering low-power idle states and increases thermal load.
- **Remediation**: Dynamically sleep based on LVGL's return value: `usleep(std::min(std::max(sleep_ms, 5u), 30u) * 1000)`.

### 2.3 $O(N)$ Unbounded Message Queue Overhead in `aasdk::Messenger`
- **Location**: [`custom_ui/third_party/aasdk/src/Messenger/Messenger.cpp:56-65, 73-80`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/third_party/aasdk/src/Messenger/Messenger.cpp#L56-L65)
- **Mechanism**:
  - When video backpressure was disabled by the cumulative ACK bug in `video_channel.cpp`, encrypted video frames flooded faster than the Hantro VPU could decode them.
  - Unconsumed messages accumulated in `Messenger::channelReceiveMessageQueue_` and `channelSendPromiseQueue_`.
  - Traversing and reallocating these vectors under continuous insertion added CPU pressure to the already-stressed single-core processor.
- **Remediation**: Ensure strict delta token flow control (`ack.set_ack(1)`), preventing queue buildup beyond the protocol sliding window.

### 2.4 Non-Blocking Serial UART Read Spin on `EAGAIN`
- **Location**: [`custom_ui/src/hal/mcu_input.cpp:120-129`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/mcu_input.cpp#L120-L129)
- **Mechanism**:
  - In `read_mcu_frame()`, if a read returns `-1` with `errno == EAGAIN` or `EINTR`, the `while` loop re-reads immediately without yielding.
- **Remediation**: Insert a 1ms sleep (`usleep(1000)`) on `EAGAIN` to prevent tight spinning if the serial port ever enters non-blocking mode.

---

## 3. Source Code Diffs

### 3.1 Rate-Limit Sidecar Spawning in `androidauto_client.cpp`
In [`custom_ui/src/hal/androidauto_client.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/androidauto_client.cpp#L54-L78):

```diff
--- a/custom_ui/src/hal/androidauto_client.cpp
+++ b/custom_ui/src/hal/androidauto_client.cpp
@@ -2,6 +2,7 @@
 
+#include <chrono>
 #include <cerrno>
 #include <cstdlib>
 #include <cstring>
@@ -54,6 +55,14 @@ namespace {
 void trySpawnSidecar() {
     std::lock_guard<std::mutex> lock(g_spawnMutex);
+
+    static auto lastSpawnAttempt = std::chrono::steady_clock::time_point::min();
+    auto now = std::chrono::steady_clock::now();
+    if (now - lastSpawnAttempt < std::chrono::seconds(5)) {
+        return; // Suppress continuous fork storms; retry at most once every 5 seconds
+    }
+    lastSpawnAttempt = now;
+
     if (std::system("pidof androidauto-sidecar >/dev/null 2>&1") == 0) {
         return;  // already running
     }
```

### 3.2 Dynamic Sleep in LVGL Main Loop (`main.cpp`)
In [`custom_ui/src/main.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/main.cpp#L466-L494):

```diff
--- a/custom_ui/src/main.cpp
+++ b/custom_ui/src/main.cpp
@@ -1,5 +1,6 @@
 #include <cstdio>
 #include <cstdlib>
+#include <algorithm>
 #include <chrono>
 #include <atomic>
 #include <thread>
@@ -466,7 +467,7 @@ int main() {
     while (true) {
-        lv_timer_handler();
+        uint32_t sleep_ms = lv_timer_handler();
         // Cheap atomic exchange every iteration -- see
         // AaAutoStartWatcher::run()'s own comment for why this can't
         // just call core::navigation::push() directly from its own
@@ -490,7 +491,8 @@ int main() {
         if (aa_auto_start_watcher().consume_navigate_request() &&
             !hal::androidauto_screen_active().load(std::memory_order_acquire)) {
             staging_ui::navigate_to(staging_ui::NavDestination::AndroidAuto);
         }
-        usleep(5000);
+        // Dynamic sleep: 5ms minimum when animating/polling, up to 30ms when static
+        usleep(std::min(std::max(sleep_ms, 5u), 30u) * 1000);
     }
 
     return 0;
```

### 3.3 Yield on `EAGAIN` in `mcu_input.cpp`
In [`custom_ui/src/hal/mcu_input.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/mcu_input.cpp#L120-L129):

```diff
--- a/custom_ui/src/hal/mcu_input.cpp
+++ b/custom_ui/src/hal/mcu_input.cpp
@@ -124,6 +124,8 @@ int read_mcu_frame(int fd, unsigned char * out_cmd, unsigned char * out_payload
         } else if (n == 0) {
             return -1;
         } else if (errno != EAGAIN && errno != EINTR) {
             break;
+        } else if (errno == EAGAIN) {
+            usleep(1000);
         }
     }
```

---

## 4. Verification & Testing Checklist

1. **Disconnected Sidecar CPU Profiling**:
   - Navigate to the Android Auto screen with no phone connected and `androidauto-sidecar` killed.
   - Run `top -d 1` or `pidstat 1` via SSH.
   - Verify that `custom_ui` CPU consumption remains $< 5\%$ and no `pidof` or `/bin/sh` processes appear in the process list.
2. **Idle UI CPU Baseline**:
   - Leave `custom_ui` idling on the Home dashboard or Settings screen.
   - Confirm CPU usage remains at $< 3\%$ with the dynamic LVGL sleep implementation.
3. **Recovery After Crash**:
   - Kill `androidauto-sidecar` with `kill -9`.
   - Verify that `custom_ui` cleanly re-attempts spawning after 5 seconds without freezing the UI thread.
