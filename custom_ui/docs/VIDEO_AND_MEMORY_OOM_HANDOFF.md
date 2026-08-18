# Handoff: Android Auto Sidecar OOM Killer Crash & Video Credit Inflation

## 1. Executive Summary

During wireless Android Auto sessions with audio/video streaming, `androidauto-sidecar` is forcefully terminated by the Linux kernel Out-Of-Memory (OOM) killer after ~2 minutes of operation:

```text
[  138.768651] blueware invoked oom-killer: gfp_mask=0x6200ca(GFP_HIGHUSER_MOVABLE), nodemask=(null), order=0, oom_score_adj=0
[  138.861117] [    205]     0   205    18490     5050    40960        0             0 androidauto-sid
[  138.895135] Out of memory: Kill process 205 (androidauto-sid) score 116 or sacrifice child
[  138.903542] Killed process 205 (androidauto-sid) total-vm:73960kB, anon-rss:19960kB, file-rss:108kB, shmem-rss:132kB
[  138.939516] oom_reaper: reaped process 205 (androidauto-sid), now anon-rss:0kB, file-rss:0kB, shmem-rss:132kB
[    0.000133] androidauto-sidecar: another instance already holds /tmp/androidauto-sidecar.lock -- refusing to start a second one
[  134.365250] hal::display::show_display: ioctl(ARKFB_SHOW_WINDOW_REAL) on /dev/fb0: ok
```

- **Process RSS at Kill**: 73.9 MB virtual memory, ~20 MB anonymous RSS (on an SoC with total system RAM of only 64MB / 128MB).
- **Secondary Lock Failure**: After the SIGKILL, respawn attempts fail in a tight loop because `/tmp/androidauto-sidecar.lock` remains held/uncleaned.

---

## 2. Root Cause Analysis

### 2.1 Video ACK Credit Inflation Bug
- **Location**: [`custom_ui/src/androidauto/video_channel.cpp:247-260`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/video_channel.cpp#L247-L260)
  ```cpp
  void VideoChannel::sendAck() {
      ++ackCount_;
      aap_protobuf::service::media::source::message::Ack ack;
      ack.set_session_id(sessionId_);
      ack.set_ack(static_cast<uint32_t>(ackCount_)); // <--- CRITICAL BUG: CUMULATIVE COUNTER
      ...
  }
  ```
- **Mechanism**:
  - In Android Auto Protocol (AAP), the `ack` field in `MediaAckIndication` represents **replenishment delta credits** (number of frames processed), **not** a cumulative frame sequence number.
  - At 30fps video, sending `ackCount_` (1, 2, 3, ... 300, ... 3000) tells Google Gearhead: *"You now have 300 additional frame credits! Now 301! Now 302!"*
  - This completely destroys all video flow control and backpressure. The phone dumps unthrottled H.264 video NALUs across WiFi at maximum wire speed.

### 2.2 Unbounded Queue Growth in `aasdk::Messenger`
- **Location**: [`custom_ui/third_party/aasdk/src/Messenger/Messenger.cpp:56-65, 76-80`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/third_party/aasdk/src/Messenger/Messenger.cpp#L56-L65)
- **Mechanism**:
  - As video frames flood in at 50–100 Mbps while the single-core Cortex-A5 Hantro decoder decodes them at 30 fps, incoming encrypted frames accumulate in `Messenger::channelReceiveMessageQueue_`.
  - Concurrently, sending 30–60 ACK promises per second queues outbound packets into `channelSendPromiseQueue_`.
  - Memory consumption escalates rapidly until the sidecar consumes ~74MB of RAM, exhausting available physical memory and triggering the kernel OOM killer.

### 2.3 Stale Flocks on SIGKILL
- When the Linux kernel OOM killer strikes, it sends an uncatchable `SIGKILL` (`kill -9`).
- C++ destructors and cleanup routines in `main.cpp` do not run, leaving `/tmp/androidauto-sidecar.lock` in an inconsistent state if flock ownership is not dynamically managed by the supervisor.

---

## 3. Remediation Plan & Source Diffs

### 3.1 Fix Video Delta Acknowledgment in `VideoChannel`
In [`custom_ui/src/androidauto/video_channel.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/video_channel.cpp#L247-L260):

```diff
--- a/custom_ui/src/androidauto/video_channel.cpp
+++ b/custom_ui/src/androidauto/video_channel.cpp
@@ -245,10 +245,9 @@ void VideoChannel::pushDecodedFrame() {
 }
 
 void VideoChannel::sendAck() {
-    ++ackCount_;
     aap_protobuf::service::media::source::message::Ack ack;
     ack.set_session_id(sessionId_);
-    ack.set_ack(static_cast<uint32_t>(ackCount_));
+    ack.set_ack(1); // Delta token (1 frame processed), NOT cumulative count
 
     auto promise = aasdk::channel::SendPromise::defer(strand_);
     promise->then(
```

### 3.2 Remove Unused `ackCount_` from `VideoChannel.h`
In [`custom_ui/src/androidauto/video_channel.h`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/video_channel.h#L73):

```diff
--- a/custom_ui/src/androidauto/video_channel.h
+++ b/custom_ui/src/androidauto/video_channel.h
@@ -70,7 +70,6 @@ private:
     bool videoLayerConfigured_ = false;
 
     int32_t sessionId_ = 0;
-    size_t ackCount_ = 0;
 };
```

---

## 4. Verification & Testing Checklist

1. **Memory Profiling Under Sustained Streaming**:
   - Start wireless Android Auto session with Spotify music playing and Google Maps 30fps navigation active.
   - Run `top` / `ps -o pid,vsz,rss,comm` via SSH on the target hardware:
     ```sh
     while true; do ps -o pid,vsz,rss,comm | grep androidauto; sleep 5; done
     ```
   - Verify that `androidauto-sidecar` memory usage stabilizes at **$< 15\text{MB}$ RSS** and does not grow linearly over time.
2. **Video Frame Pacing & Stability**:
   - Confirm video remains smooth at 30 fps without packet queue accumulation.
   - Verify that OOM killer does not trigger throughout a 30+ minute drive test.
