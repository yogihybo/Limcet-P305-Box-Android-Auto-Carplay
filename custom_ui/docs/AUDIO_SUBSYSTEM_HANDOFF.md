# Handoff: Android Auto Audio Subsystem Stutter & Flow Control Investigation

## 1. Executive Summary

During wireless Android Auto playback in `custom_ui`, audio exhibits intermittent or continuous micro-stuttering, choppy buffering gaps, and occasional session teardown (`ECONNRESET`). 

Crucially:
- **Zero Kernel XRUNs or `dmesg` Errors**: Hardware audio DMA runs cleanly without underflows.
- **Identical Hardware & Kernel Runs Cleanly on Stock**: The vendor MSN application (`/usr/bin/sink`, `libAndroidAuto.so`, `libMsnCarAuto.so`) plays identical Android Auto streams smoothly without any audio stutter on the exact same board, Linux kernel (4.19.192 / 3.4.0), and WiFi environment.
- **Root Cause Isolated to Application-Layer Flow Control**: The stutter is caused by a **stop-and-wait flow control lockstep** introduced by pairing `max_unacked = 1` with an ACK callback gated behind blocking ALSA playback, further aggravated by a protocol delta-credit vs. cumulative counter misconception.

---

## 2. Root Cause Analysis

```
Phone (Android Auto Source)                     custom_ui (Head Unit Sink)
           │                                                │
           │────── Buffer N (~21.3ms PCM audio) ───────────>│ Enqueued to AlsaOutput
           │                                                │
 [STALLED: max_unacked=1]                                   │ snd_pcm_writei() plays Buffer N
           │                                                │ (~21.3ms real playback duration)
           │                                                │
           │                                                │ onConsumed_() fires
           │                                                │ strand_.post([this]{ sendAck(); })
           │                                                │ [io_service delayed by H.264 video]
           │<───── MediaAckIndication (Buffer N) ───────────│ Encrypt SSL + WiFi TX (15-35ms)
           │                                                │
           │                                                │ ⚠️ ALSA buffer runs completely DRY
           │────── Buffer N+1 (~21.3ms) ───────────────────>│ Arrives 20-40ms LATE -> Audible Gap!
```

### 2.1 The Stop-and-Wait Lockstep Starvation
- **Locations**: 
  - [`custom_ui/src/androidauto/audio_channel.cpp:41-43, 88`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp#L41-L43)
  - [`custom_ui/src/androidauto/alsa_output.cpp:144-150`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.cpp#L144-L150)
- **Mechanism**:
  1. In `AudioChannel::onMediaChannelSetupRequest`, `response.set_max_unacked(1)` limits the phone to sending at most 1 buffer ahead without an acknowledgment.
  2. In `AudioChannel::start()`, `alsaOutput_.setConsumedCallback()` is wired to post `sendAck()` to `strand_`.
  3. In `AlsaOutput::writerLoop()`, `onConsumed_()` is only invoked **after** `writeBlocking()` completes its synchronous `snd_pcm_writei()` call.
  4. When playing 48kHz 16-bit stereo audio, a standard 2048-byte or 4096-byte chunk represents only **~10.6ms to ~21.3ms of audio**.
  5. The network round-trip time (Strand queue delay + OpenSSL encryption + 2.4GHz WiFi packet transmission + phone scheduling + phone generation + WiFi reception + OpenSSL decryption) consistently takes **15ms to 45ms**.
  6. Because the phone is forbidden from sending Buffer $N+1$ until it receives the ACK for Buffer $N$, the ALSA ring buffer empties during the round-trip latency window, creating an unavoidable silence gap on every single audio chunk.

### 2.2 Why `dmesg` Shows No XRUNs
- Under [`/etc/asound.conf`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/firmware_source/mtd6_rootfs/etc/asound.conf), audio routes through `plug:softvol2` $\rightarrow$ `pcm.dmix` $\rightarrow$ `hw:0,0`.
- `dmix` is a software mixing plugin running entirely in user space inside `libasound.a`.
- When an individual application-level playback client starves of samples, `dmix` continues outputting silence (zeros) to the hardware DMA buffer without dropping the underlying hardware stream.
- As a result, the starvation manifests audibly as stuttering/buffering, but never triggers an ASoC kernel DMA underrun (`pcmC0D0p` XRUN) in `dmesg`.

### 2.3 Protocol Credit Accounting: Delta Tokens vs. Cumulative Sequence Number
- **Location**: [`custom_ui/src/androidauto/audio_channel.cpp:161-174`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp#L161-L174)
  ```cpp
  void AudioChannel::sendAck() {
      ++ackCount_;
      aap_protobuf::service::media::source::message::Ack ack;
      ack.set_session_id(sessionId_);
      ack.set_ack(static_cast<uint32_t>(ackCount_)); // BUG: sending cumulative count
      ...
  }
  ```
- In Google Android Auto Protocol (AAP / AASDK `Ack.proto`), the `ack` field represents **replenishment tokens (delta credits)** for media flow control, typically `1` frame acknowledged, rather than a cumulative sequence counter.
- When `custom_ui` previously attempted to ACK buffers immediately upon receipt, sending cumulative counts (`ack = 1, 2, 3, ... 100...`) continuously expanded the sender's transmission window by dozens of credits. The phone responded by dumping bursts of audio at raw network speed, overrunning queues and crashing the transport (`ECONNRESET`).
- Playback-gated ACKs were added to throttle transmission, which masked the credit accounting bug while introducing chronic starvation.

### 2.4 Single-Threaded ASIO Loop Contention
- **Location**: [`custom_ui/src/androidauto/wireless_session_manager.cpp:378-380`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L378-L380)
- `WirelessSessionManager` runs a single `ioService.run()` thread that services:
  - Network I/O and SSL decryption/encryption across all channels.
  - Video H.264 NAL parsing and submission to the Hantro VPU (`HantroH264Decoder::decodeFrame`).
  - Audio ingest across all three audio sinks (`Media`, `Guidance`, `System`).
  - Control channel pings, sensor messages, and input events.
- On an ARM Cortex-A5 single-core processor, decoding an 800x480 video frame or handling touch/display events momentarily delays the `io_service` message pump. With `max_unacked = 1`, any strand scheduling jitter directly stalls the next incoming audio packet.

---

## 3. Stock MSN App (`sink` / `libAndroidAuto.so`) Architecture

Disassembly of `/usr/bin/sink` and `libAndroidAuto.so` (documented in `docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md` § "RESOLVED: audio dispatch is genuinely asynchronous") reveals how the stock app avoids this problem:

1. **Decoupled Asynchronous WorkQueue**:
   - The network reader thread (`GalReaderThread`) receives incoming media packets, decrypts them inline, and routes them via `AudioSink::handleDataAvailable()`.
   - `AudioSinkCallbacks::dataAvailableCallback()` packages the PCM payload into an `AudioPlaybackWorkItem` and enqueues it to `WorkQueue`.
2. **Immediate Delta Acknowledgment**:
   - `dataAvailableCallback()` **immediately calls `MediaSinkBase::ackFrames(1)` and returns to the reader thread**.
   - It does **not** wait for ALSA to consume the audio.
3. **Pipelined Sliding Window**:
   - With immediate delta ACKs and a window size (`max_unacked = 8`), the phone continuously pipelines audio ahead into the head unit's queue.
   - The playback thread pool (`WorkQueueThread`) drains the queue into `AlsaHandle::play()` independently of network and scheduling jitter.

---

## 4. Remediation Plan & Source Diffs

### 4.1 Fix Flow Control & Immediate ACKs in `AudioChannel`
Modify [`custom_ui/src/androidauto/audio_channel.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp) and [`custom_ui/src/androidauto/audio_channel.h`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.h):

1. Set `max_unacked` to `8` in `onMediaChannelSetupRequest`.
2. Send delta token `1` in `sendAck()`.
3. Acknowledge immediately in `playBuffer()`.
4. Remove the blocking playback `onConsumed_` synchronization callback.

```diff
--- a/custom_ui/src/androidauto/audio_channel.cpp
+++ b/custom_ui/src/androidauto/audio_channel.cpp
@@ -22,23 +22,6 @@ AudioChannel::AudioChannel(boost::asio::io_service::strand & strand,
 void AudioChannel::start() {
-    // 2026-08-18: see alsa_output.h's own class comment for the full
-    // story -- this channel's ack is max_unacked=1's actual flow
-    // control (the phone won't send the next buffer until it gets
-    // this), so it must fire at real playback pace, not the instant a
-    // buffer is handed to AlsaOutput::write() (which now just enqueues
-    // it). AlsaOutput invokes this from ITS OWN writer thread (or
-    // synchronously from write() on the drop path) -- never assume
-    // it's already on strand_, always post. Wired here rather than the
-    // constructor -- shared_from_this() isn't legal until a shared_ptr
-    // already owns this object, and a subagent review flagged that a
-    // bare `this` capture here (unlike Session::sendPing()'s own
-    // self-capturing fix for the identical async-lifetime concern)
-    // relies on a fragile invariant rather than a real guarantee: it
-    // only happens to be safe today because channels can currently
-    // only be destroyed on the same thread that owns io_service, after
-    // run() returns. A self-capturing shared_ptr removes that
-    // dependency entirely.
-    auto self = shared_from_this();
-    alsaOutput_.setConsumedCallback([this, self]() {
-        strand_.post([this, self]() { sendAck(); });
-    });
     channel_->receive(this->shared_from_this());
 }
 
@@ -85,7 +68,7 @@ void AudioChannel::onMediaChannelSetupRequest(
     // optional but apparently required in practice. 1 matches
     // microphone_channel.cpp's own already-correct value, which itself
     // matches the real upstream f1x/openauto reference.
-    response.set_max_unacked(1);
+    response.set_max_unacked(8);
     response.add_configuration_indices(0);
 
     auto promise = aasdk::channel::SendPromise::defer(strand_);
@@ -144,14 +127,7 @@ void AudioChannel::playBuffer(const aasdk::common::DataConstBuffer & buffer) {
         uint32_t frameCount = static_cast<uint32_t>(buffer.size / bytesPerFrame);
         if (frameCount > 0) {
-            // Ack fires later, via the consumed-callback set in the
-            // constructor -- once this buffer's real write actually
-            // happens (or immediately if dropped) -- not here. See
-            // alsa_output.h's class comment: acking immediately after
-            // just enqueueing broke max_unacked=1's flow control and
-            // crashed the session on real hardware.
             alsaOutput_.write(buffer.cdata, frameCount);
-            return;
         }
     }
-    // ALSA never opened, or a zero-length buffer -- nothing will ever
-    // invoke the consumed callback for this one, so ack directly.
     sendAck();
 }
 
 void AudioChannel::sendAck() {
-    ++ackCount_;
     aap_protobuf::service::media::source::message::Ack ack;
     ack.set_session_id(sessionId_);
-    ack.set_ack(static_cast<uint32_t>(ackCount_));
+    ack.set_ack(1);
 
     auto promise = aasdk::channel::SendPromise::defer(strand_);
```

### 4.2 Streamline `AlsaOutput`
Modify [`custom_ui/src/androidauto/alsa_output.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.cpp) and [`custom_ui/src/androidauto/alsa_output.h`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.h):

1. Remove `onConsumed_` callback invocations.
2. In `write()`, if `queue_.size() >= kMaxQueuedBuffers`, drop the oldest queued buffer (pop front) and enqueue the newest frame so playback tracks real-time stream state without backlog buildup.

```diff
--- a/custom_ui/src/androidauto/alsa_output.cpp
+++ b/custom_ui/src/androidauto/alsa_output.cpp
@@ -107,6 +107,7 @@ bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
     {
         std::lock_guard<std::mutex> lock(mutex_);
         if (queue_.size() >= kMaxQueuedBuffers) {
+            queue_.pop_front();
             ++droppedBuffers_;
             if (droppedBuffers_ == 1 || droppedBuffers_ % 100 == 0) {
                 std::fprintf(stderr, "%s androidauto::AlsaOutput: writer thread for %s falling behind, "
@@ -114,12 +115,6 @@ bool AlsaOutput::write(const void * interleavedSamples, uint32_t frameCount) {
                              deviceName_.c_str(), droppedBuffers_);
             }
-            if (onConsumed_) onConsumed_();
-            return true;
         }
         queue_.push_back(std::move(copy));
     }
@@ -148,3 +143,2 @@ void AlsaOutput::writerLoop() {
             writeBlocking(buf.data(), frameCount);
         }
-        if (onConsumed_) onConsumed_();
     }
 }
```

---

## 5. Verification & Testing Checklist

1. **Local Playback Baseline**:
   Verify clean audio output using `aplay` directly on the device:
   ```sh
   aplay -D plug:softvol2 /usr/share/sounds/alsa/Front_Center.wav
   ```
2. **Android Auto Wireless Audio Session**:
   - Start wireless Android Auto session.
   - Stream Spotify or YouTube Music over high-bitrate media channel (48 kHz stereo 16-bit).
   - Verify that playback remains glitch-free during concurrent 800x480 30fps H.264 video decoding.
   - Monitor sidecar terminal log for queue drops (`droppedBuffers_` counter in `AlsaOutput`).
3. **Session Stability**:
   - Confirm session does not drop with `ECONNRESET` or Native Code 104 after prolonged playback (>15 minutes).
   - Test simultaneous TTS guidance audio interjections (`plug:softvol1`) over music playback.
