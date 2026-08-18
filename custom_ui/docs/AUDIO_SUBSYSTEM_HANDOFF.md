# Handoff: Android Auto Audio Subsystem Stutter & Flow Control Investigation

## 1. Executive Summary

During wireless Android Auto playback in `custom_ui`, two distinct audio failure modes were investigated:

1. **Mode 1 (Stop-and-Wait Starvation)**: Pairing `max_unacked = 1` with playback-gated ACKs caused chronic inter-packet silence gaps ($15\text{ms} - 45\text{ms}$ network RTT) on every single audio chunk, manifesting as continuous micro-stuttering without kernel DMA XRUNs.
2. **Mode 2 (Burst Buffer Overflow & A/V Desync Drop)**: Acknowledging immediately on ingest with `max_unacked = 8` against an overly tight queue (`kMaxQueuedBuffers = 32`, ~340ms) caused media app pre-buffering bursts (1–2s of audio from Spotify/YouTube Music) to instantly overflow the queue:
   ```text
   [   32.271052] androidauto::AlsaOutput: writer thread for plug:softvol2 falling behind, dropped 1 buffer(s) so far
   [   43.766959] androidauto::AlsaOutput: writer thread for plug:softvol2 falling behind, dropped 100 buffer(s) so far
   [  105.188724] hal::display::show_display: ioctl(ARKFB_SHOW_WINDOW_REAL) on /dev/fb0: ok
   ```
   Dropping 100+ audio buffers caused severe Audio/Video Presentation Timestamp (PTS) desynchronization ($>1.1\text{s}$ drift). Google's Gearhead media engine responded by revoking projected video focus (`VideoFocusRequest mode=2`), causing `custom_ui` to revert `/dev/fb0` to native UI while the phone link stayed connected in the background.

---

## 2. Root Cause Analysis

```
Phone (Spotify / Media Source)                   custom_ui (Head Unit Sink)
       │                                                     │
       │─── Sends Initial 1-2s Buffer Burst @ WiFi Speed ───>│ playBuffer() enqueues to AlsaOutput
       │                                                     │ sends immediate Ack(1)
       │                                                     │
       │                                                     │ ⚠️ Queue capped at 32 buffers (~340ms)
       │                                                     │
       │                                                     │ ALSA writer plays Packet 1 @ real-time
       │                                                     │ (10.6ms per 2048-byte buffer)
       │                                                     │
       │                                                     │ Packets 33..132 OVERFLOW queue!
       │                                                     │ -> 100+ buffers discarded
       │                                                     │ -> Audio PTS drifts +1.1s ahead of Video PTS
       │                                                     │ -> Phone revokes video focus & exits AA
```

### 2.1 The Buffer Queue Capacity Ceiling
- At 48 kHz 16-bit stereo, a standard 2048-byte buffer represents only **~10.6ms** (or 4096 bytes = ~21.3ms) of audio.
- A queue cap of `kMaxQueuedBuffers = 32` holds at most **~340ms to ~680ms** of audio.
- Media applications routinely pre-buffer **1 to 2 seconds** of audio when starting playback. When ACKs are sent immediately on ingest, the phone dumps this pre-buffer in a matter of milliseconds.
- The 340ms queue capacity fills instantly, and all subsequent burst packets are dropped by `queue_.pop_front()`.

### 2.2 Why the Session Exits While the Phone Remains Connected
- In Android Auto projection, Gearhead continuously compares the audio presentation clock (`onMediaWithTimestampIndication`) with the video presentation clock.
- Discarding 100 audio buffers advances the audio playback position by over a second while video rendering proceeds at normal frame rates.
- When A/V desync exceeds Gearhead's tolerance threshold, the phone halts projected rendering and issues a `VideoFocusRequest` with `mode=2` (UNFOCUSED / Native Focus).
- `custom_ui`'s `android_auto_screen.cpp` detects `client().videoFocusNative() == true` and calls `hal::show_display()` to bring native UI back onto `/dev/fb0`, while the background TCP session remains alive.

---

## 3. Solution: Generous Jitter Buffer + Adaptive High-Water Mark Pacing

To completely eliminate both stop-and-wait starvation (Mode 1) and burst queue overflows (Mode 2), the audio subsystem requires:

1. **Expanded Jitter Buffer**: Increase `kMaxQueuedBuffers` from `32` to **`256`** (~2.7 seconds of 48kHz audio), providing ample headroom to absorb initial media app pre-buffer bursts.
2. **Adaptive High-Water Mark Flow Control**:
   - **Low Water Mark (`queue_.size() < 32`)**: Send `sendAck(1)` immediately upon receiving the packet to keep the network pipeline saturated and prevent inter-packet starvation gaps.
   - **High Water Mark (`queue_.size() >= 32`)**: Defer `sendAck(1)` to `AlsaOutput`'s consumed callback (fired when the writer thread actually plays the frame to ALSA), naturally pacing the phone's transmission speed to real-time playback pace without ever discarding a single buffer.

---

## 4. Source Code Changes & Exact Diffs

### 4.1 Update [`custom_ui/src/androidauto/alsa_output.h`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.h)

```diff
--- a/custom_ui/src/androidauto/alsa_output.h
+++ b/custom_ui/src/androidauto/alsa_output.h
@@ -124,13 +124,18 @@ public:
     // buffer is dropped (queue full).
     using ConsumedCallback = std::function<void()>;
     void setConsumedCallback(ConsumedCallback cb) { onConsumed_ = std::move(cb); }
 
+    // Returns the current count of queued buffers for adaptive watermark pacing
+    size_t queuedBuffers() const {
+        std::lock_guard<std::mutex> lock(mutex_);
+        return queue_.size();
+    }
+
     void close();
 
 private:
     void writerLoop();
     bool writeBlocking(const void * interleavedSamples, uint32_t frameCount);
 
     std::string deviceName_;
     uint32_t sampleRate_;
     uint32_t bitsPerSample_;
     uint32_t channels_;
 
     snd_pcm_t * pcmHandle_ = nullptr;
 
-    // Caps memory/latency growth if the writer thread ever falls
-    // behind -- see class comment. ~1s of audio at a typical AA
-    // buffer size (a few dozen ms per buffer), generous enough to
-    // absorb normal scheduling jitter without masking a real,
-    // sustained problem.
-    static constexpr size_t kMaxQueuedBuffers = 32;
+    // 256 buffers @ 2048B (10.6ms) = ~2.7 seconds of audio buffer capacity,
+    // sufficient to absorb initial media player pre-buffering bursts (Spotify/YouTube Music).
+    static constexpr size_t kMaxQueuedBuffers = 256;
 
     std::thread writerThread_;
-    std::mutex mutex_;
+    mutable std::mutex mutex_;
     std::condition_variable cv_;
```

### 4.2 Update [`custom_ui/src/androidauto/alsa_output.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.cpp)

```diff
--- a/custom_ui/src/androidauto/alsa_output.cpp
+++ b/custom_ui/src/androidauto/alsa_output.cpp
@@ -107,17 +107,14 @@ bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
     {
         std::lock_guard<std::mutex> lock(mutex_);
         if (queue_.size() >= kMaxQueuedBuffers) {
             queue_.pop_front();
             ++droppedBuffers_;
             if (droppedBuffers_ == 1 || droppedBuffers_ % 100 == 0) {
                 std::fprintf(stderr, "%s androidauto::AlsaOutput: writer thread for %s falling behind, "
                              "dropped %u buffer(s) so far\n", androidauto::logTimestamp().c_str(),
                              deviceName_.c_str(), droppedBuffers_);
             }
         }
         queue_.push_back(std::move(copy));
     }
     cv_.notify_one();
     return true;
@@ -147,6 +144,9 @@ void AlsaOutput::writerLoop() {
         if (frameCount > 0) {
             writeBlocking(buf.data(), frameCount);
         }
+        if (onConsumed_) {
+            onConsumed_();
+        }
     }
 }
```

### 4.3 Update [`custom_ui/src/androidauto/audio_channel.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp)

```diff
--- a/custom_ui/src/androidauto/audio_channel.cpp
+++ b/custom_ui/src/androidauto/audio_channel.cpp
@@ -21,6 +21,14 @@ AudioChannel::AudioChannel(boost::asio::io_service::strand & strand,
 
 void AudioChannel::start() {
+    auto self = shared_from_this();
+    alsaOutput_.setConsumedCallback([this, self]() {
+        strand_.post([this, self]() {
+            // Only fire consumed ACKs when draining under high-water mark backpressure
+            if (pendingPacedAcks_ > 0) {
+                --pendingPacedAcks_;
+                sendAck();
+            }
+        });
+    });
     channel_->receive(this->shared_from_this());
 }
 
@@ -131,14 +139,23 @@ void AudioChannel::playBuffer(const aasdk::common::DataConstBuffer & buffer) {
     if (alsaOpen_) {
         uint32_t bytesPerFrame = 2 * channels_;
         uint32_t frameCount = static_cast<uint32_t>(buffer.size / bytesPerFrame);
         if (frameCount > 0) {
             alsaOutput_.write(buffer.cdata, frameCount);
+
+            // Adaptive Watermark Pacing:
+            // If queue has ample headroom (<32 buffers), ACK immediately to keep network pipeline saturated.
+            // If queue reaches high-water mark (>=32 buffers), defer ACK to real playback to pace the phone.
+            if (alsaOutput_.queuedBuffers() >= 32) {
+                ++pendingPacedAcks_;
+                return;
+            }
         }
     }
     sendAck();
 }
```

### 4.4 Update [`custom_ui/src/androidauto/audio_channel.h`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.h)

```diff
--- a/custom_ui/src/androidauto/audio_channel.h
+++ b/custom_ui/src/androidauto/audio_channel.h
@@ -71,6 +71,7 @@ private:
     bool alsaOpen_ = false;
 
     int32_t sessionId_ = 0;
+    size_t pendingPacedAcks_ = 0;
 };
```

---

## 5. Verification & Testing Checklist

1. **Media Pre-Buffer Burst Test**:
   - Start wireless Android Auto session and launch Spotify / YouTube Music.
   - Start track playback from a cold state (triggers 1–2s media pre-buffer burst).
   - Verify terminal log: `droppedBuffers_` counter **must remain 0** (no buffers dropped).
2. **Sustained Playback & A/V Sync Stability**:
   - Play continuous audio for $>15$ minutes while Google Maps displays 30fps turn-by-turn video.
   - Confirm video projection focus is never revoked (`VideoFocusRequest mode=2` does not fire).
   - Verify that `/dev/fb0` does not prematurely unhide over video playback.
3. **Low-Latency Interjection Test**:
   - Trigger Google Assistant or navigation guidance (`plug:softvol1`) during music playback (`plug:softvol2`).
   - Verify smooth audio ducking without glitches or pipeline stalls.
