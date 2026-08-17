# Handoff: Android Auto Video Pipeline & HAL Issues

## 1. Executive Summary

This handoff document details the technical investigation into the **whitish/grey translucent overlay** observed on the Android Auto video feed, along with deep-dive audit findings across the `custom_ui` HAL, sidecars, and UI layers (video decode, ALSA audio, stream IPC, Bluetooth daemon management, MCU serial input, and display layer compositing).

---

## 2. Primary Investigation: Video Overlay / Grey Wash

### 2.1 Phenomenon & Symptoms
* A grey/white translucent wash covers the entire Android Auto video surface for the first several seconds of each session.
* Regions clear progressively as new UI animations or updates occur; static areas retain the wash.
* The Hantro decoder reports `nbrOfErrMBs = 0` and claims pictures are ready (`picId=3` on frame #1).
* Hardware diagnostic logged: `picId=3, isIdr=1500 (0x05DC), codingType=0`.

### 2.2 Root Cause Analysis & Decompile Findings

#### A. `h264bsdConceal()` Stamping Canned Fill Patterns
* **Ghidra Finding**: `libmfc.so` (Hantro G1 DWL) contains `h264bsdConceal()`. When decode is attempted before reference pictures or parameter sets are committed in the DPB (Decoded Picture Buffer), it fills macroblocks with a canned pattern.
* **Why `nbrOfErrMBs == 0`**: The decoder treats concealed macroblocks as handled rather than reporting a decoding failure.
* **Why `isIdr=1500`**: Traces to an uninitialized DPB bookkeeping array slot populated separately from the bitstream header parser.

#### B. Re-entrant Bitstream Consumption in `H264DecDecode()`
* **Mechanism**: In [`custom_ui/src/androidauto/hantro_h264_decoder.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_h264_decoder.cpp#L255-L299), `h264DecDecode_` is called once per `decodeFrame()` invocation.
* When the initial stream packet containing `SPS (7) + PPS (8) + IDR (5)` arrives, `H264DecDecode` parses SPS/PPS and returns `H264DEC_HDRS_RDY (4)` with unconsumed bytes in `output.dataLeft`.
* Without a re-entrant loop consuming `output.pStrmCurrPos` / `output.dataLeft`, the initial IDR slice is dropped before the next incoming P-frame overwrites the DMA buffer, forcing the decoder to conceal frame #2 against an uninitialized reference.

#### C. Display Controller Blending State (`ARKFB_SET_BLEND`)
* In [`custom_ui/src/hal/video_layer.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/video_layer.cpp#L207-L216), `ARKFB_SET_BLEND` was omitted after switching to `ARK_INIT_FB_DISPLAY` (`0x403c4f27`).
* If `MODE_LCD_REG1` retains `alpha_blend_en = 1` or non-zero blend modes on Video Layer 2 (`/dev/fb4`), the hardware LCDC composites decoded frames against `BACK_COLOR_REG` (`LCDC + 0x50`, default grey/white `0x00808080` / `0x00FFFFFF`).

### 2.3 Diagnostic Verification Plan
The `logInputNalTypes` helper in [`hantro_h264_decoder.cpp:236-252`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_h264_decoder.cpp#L236-L252) logs raw NAL types for the first 10 frames:
* **Outcome 1**: `picId=1` logs types `7`, `8`, and `5` $\rightarrow$ Confirms SPS+PPS+IDR bundling in frame 1; requires `output.dataLeft` loop.
* **Outcome 2**: `picId=1` logs only `7` and `8`; `5` arrives later $\rightarrow$ Message framing separation; decode must defer until IDR arrives.

---

## 3. Comprehensive Codebase Audit Findings

### 3.1 DMA Buffer Alignment Headroom Calculation
* **Location**: [`custom_ui/src/androidauto/hantro_h264_decoder.cpp:142-158`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_h264_decoder.cpp#L142-L158)
* **Issue**: Calculating 50% headroom via `uint32_t grown = aligned + aligned / 2;` produces non-page-aligned sizes (e.g. 4096 $\rightarrow$ 6144), causing non-page-multiple `mmap`/`munmap` calls against the kernel DMA allocator.
* **Resolution**: Re-align `grown` to `pagesize`:
  ```cpp
  uint32_t grown = aligned + aligned / 2;
  if (grown > aligned) aligned = (grown + pagesize - 1) & ~(pagesize - 1);
  ```

### 3.2 ALSA Output Partial Write Handling
* **Location**: [`custom_ui/src/androidauto/alsa_output.cpp:153-172`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.cpp#L153-L172)
* **Issue**: `snd_pcm_writei()` can return `0 < written < frameCount`. The current implementation returns `true` without looping for the remaining samples, silently dropping partial audio buffers and causing clicks/pops.
* **Resolution**: Implement a write loop consuming `remaining` frames while advancing `interleavedSamples`.

### 3.3 Stream Socket Partial Reads in Handshake Client
* **Location**: [`custom_ui/src/androidauto/bw_aap_client.cpp:151-170`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/bw_aap_client.cpp#L151-L170)
* **Issue**: `::read(fd_, &payload[0], length)` on `SOCK_STREAM` socket `/dev/bw_aap` assumes full payload delivery in a single read syscall. Packet fragmentation triggers immediate failure.
* **Resolution**: Wrap socket reads in a loop until `total_read == length` or fatal error.

### 3.4 Microphone Channel Strand Deadlock on Teardown
* **Location**: [`custom_ui/src/androidauto/microphone_channel.cpp:247-258`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/microphone_channel.cpp#L247-L258)
* **Issue**: `stopCapture()` sets `capturing_ = false` and calls `captureThread_.join()`. Because `captureThread_` blocks in `snd_pcm_readi()`, it will not wake until data arrives or the device is closed, risking a strand hang.
* **Resolution**: Call `snd_pcm_drop()` or close the ALSA capture handle prior to joining `captureThread_`.

### 3.5 MCU UART Error Loop Throttling
* **Location**: [`custom_ui/src/hal/mcu_input.cpp:204-214`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/mcu_input.cpp#L204-L214)
* **Issue**: If `read_mcu_frame()` returns `0` (checksum mismatch) or `-1` continuously, `McuInputHal::run()` spins at 100% CPU on its thread.
* **Resolution**: Add minimal backoff (`usleep(1000)`) on read errors.

### 3.6 Unhandled `SIGPIPE` in `androidauto-sidecar` Daemon
* **Location**: [`custom_ui/sidecars/androidauto/main.cpp:202`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/sidecars/androidauto/main.cpp#L202)
* **Issue**: Multi-threaded socket writes via `::write(clientFd, ...)` without `signal(SIGPIPE, SIG_IGN)` will terminate the entire sidecar process if a client disconnects or times out mid-reply.
* **Resolution**: Add `signal(SIGPIPE, SIG_IGN);` at daemon entry.

### 3.7 Blocking `accept()` Hang on Aborted TCP Connection
* **Location**: [`custom_ui/src/androidauto/wireless_session_manager.cpp:337-353`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp#L337-L353)
* **Issue**: `select()` wakes on incoming connection, but `acceptor.accept(*socket)` runs in blocking mode. If the phone issues a TCP RST before accept executes, `accept()` blocks indefinitely, leaving the manager stuck in `Connecting`.
* **Resolution**: Set `acceptor.non_blocking(true)` before `select`/`accept`.

### 3.8 Bluetooth Reader Loop Exit on Transient Error
* **Location**: [`custom_ui/src/hal/bluetooth.cpp:103-107`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/bluetooth.cpp#L103-L107)
* **Issue**: If `::read()` returns `<= 0` (e.g. `EINTR` or `blueware` restart), `reader_loop()` exits permanently while `ReaderState::started` remains `true`. All future `send_command()` calls time out.
* **Resolution**: Re-open `/dev/bw_serial` and retry on unexpected EOF/error, or reset `rs.started = false` upon exit.

### 3.9 Key Event Down/Up Timestamp Coalescing
* **Location**: [`custom_ui/src/androidauto/input_channel.cpp:56-73`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/input_channel.cpp#L56-L73)
* **Issue**: `sendKey()` emits `down=true` and `down=false` in the same microsecond with identical timestamps. Android input dispatchers coalesce or drop identical timestamp down/up pairs.
* **Resolution**: Add a distinct timestamp delta between down and up reports.

### 3.10 Duplicate Screen Stacking on Auto-Start
* **Location**: [`custom_ui/src/main.cpp:436-438`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/main.cpp#L436-L438)
* **Issue**: When `consume_navigate_request()` fires, it calls `push(ui::create_android_auto_screen)` unconditionally, creating duplicate screens and duplicate polling timers if the user is already on the screen.
* **Resolution**: Check `core::navigation::current()` before pushing.

### 3.11 Missing `DT_INIT` Dynamic Linker Constructor Support
* **Location**: [`custom_ui/src/androidauto/hantro_dlopen.c:351-366, 492-494`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_dlopen.c#L351-L366)
* **Issue**: Custom ELF loader handles `DT_INIT_ARRAY` but ignores legacy `DT_INIT` entries in `.dynamic`. If `libmfc.so` defines `.init`, its initialization function is skipped.
* **Resolution**: Parse `DT_INIT` and invoke it prior to executing `DT_INIT_ARRAY`.

### 3.12 Misassigned AAOS Rotary Navigation Keycodes
* **Location**: [`custom_ui/src/hal/knob.cpp:16-17, 68-73`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/knob.cpp#L16-L17)
* **Issue**: Knob ticks send `KEYCODE_SYSTEM_NAVIGATION_DOWN` (281) and `KEYCODE_SYSTEM_NAVIGATION_UP` (280). Gearhead's rotary focus controller ignores system bar keycodes, causing rotation events to have no effect.
* **Resolution**: Map rotation to `KEYCODE_NAVIGATE_NEXT` (261) / `KEYCODE_NAVIGATE_PREVIOUS` (260) or D-Pad navigation (`KEYCODE_DPAD_DOWN`/`UP` or `LEFT`/`RIGHT`), and advertise them in `session.cpp`.

### 3.13 Synchronous Socket IPC Blocking the LVGL Main Thread
* **Location**: [`custom_ui/src/ui/status_bar.cpp:66`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/ui/status_bar.cpp#L66), [`custom_ui/src/hal/touch.cpp:47-51`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/touch.cpp#L47-L51)
* **Issue**: `statusLine()` and `sendTouch()` execute synchronous `read()` calls against `/tmp/androidauto-sidecar.sock` from the LVGL main UI thread during every 1s timer tick and 50–100Hz touch poll. If the sidecar is busy, the UI freezes.
* **Resolution**: Apply socket timeouts (`SO_RCVTIMEO`) or cache state asynchronously.

### 3.14 Flow Control Ack Value Counter vs Delta Tokens
* **Location**: [`custom_ui/src/androidauto/audio_channel.cpp:161-166`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/audio_channel.cpp#L161-L166), [`custom_ui/src/androidauto/video_channel.cpp:313-318`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/video_channel.cpp#L313-L318)
* **Issue**: `sendAck()` sends a cumulative counter (`ackCount_`) in `Ack.proto`. If the phone interprets `ack` as delta tokens to replenish the `max_unacked = 1` window, sending cumulative counts inflates the sender's credit buffer.
* **Resolution**: Verify whether protocol expects batch token count `1` or sequence number.

### 3.15 UART Inter-character Timeout Treated as Fatal Error
* **Location**: [`custom_ui/src/hal/mcu_input.cpp:124-125, 142-143`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/mcu_input.cpp#L124-L125)
* **Issue**: `read_mcu_frame()` returns `-1` on `n == 0` (`VTIME = 1` 100ms inter-character timeout), treating a normal frame gap/pause as a fatal EOF.
* **Resolution**: Return `0` (resync) on `n == 0` instead of `-1`.

### 3.16 Unwired Reverse Gear Watcher & Unasserted App Ready Handshake
* **Location**: [`custom_ui/src/core/reverse_gear_watcher.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/core/reverse_gear_watcher.cpp), [`custom_ui/src/main.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/main.cpp)
* **Issue**: `ReverseGearWatcher` is not instantiated in `main.cpp` and `hal::set_app_ready()` is never called, leaving the kernel `ark-carback` driver unaware of userspace handling.
* **Resolution**: Instantiate watcher in `main.cpp` and coordinate `CARBACK_IOCTL_SET_APP_READY` / `APP_ENTER_DONE`.

---

## 4. Key Files & Reference Index

| File | Subsystem | Focus Area |
| :--- | :--- | :--- |
| [`custom_ui/src/androidauto/hantro_h264_decoder.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_h264_decoder.cpp) | Video Decode | `decodeFrame`, NAL logging, DMA sizing |
| [`custom_ui/src/androidauto/video_channel.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/video_channel.cpp) | Video Pipeline | `pushDecodedFrame`, timing, diagnostics |
| [`custom_ui/src/hal/video_layer.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/video_layer.cpp) | LCDC Driver | `ARK_INIT_FB_DISPLAY`, `ARKFB_SET_BLEND` |
| [`custom_ui/sidecars/androidauto/main.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/sidecars/androidauto/main.cpp) | Sidecar IPC | `SIGPIPE` handling, socket server |
| [`custom_ui/src/androidauto/wireless_session_manager.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/wireless_session_manager.cpp) | Wireless AAP | Non-blocking accept, BSSID |
| [`custom_ui/src/hal/bluetooth.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/bluetooth.cpp) | Bluetooth HAL | Reader resilience, AT commands |
| [`custom_ui/src/androidauto/alsa_output.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/alsa_output.cpp) | Audio Out | `writeBlocking`, partial write loop |
| [`custom_ui/src/androidauto/microphone_channel.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/microphone_channel.cpp) | Audio In | `stopCapture`, thread synchronization |
| [`custom_ui/src/androidauto/bw_aap_client.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/bw_aap_client.cpp) | Wireless AAP | `receiveFrame`, stream socket loops |
| [`custom_ui/src/hal/mcu_input.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/mcu_input.cpp) | MCU Protocol | `run()`, error pacing, timeout handling |
| [`custom_ui/src/androidauto/hantro_dlopen.c`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/androidauto/hantro_dlopen.c) | Dynamic Loader | `DT_INIT` support |
| [`custom_ui/src/hal/knob.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/hal/knob.cpp) | Rotary Input | Gearhead keycode mapping (260/261) |
| [`custom_ui/src/main.cpp`](file:///c:/Users/Caleb%20Smith/Documents/GitHub/prado-firmware-reconstruction/custom_ui/src/main.cpp) | Main UI Loop | Auto-start navigation deduplication, ReverseGearWatcher |
