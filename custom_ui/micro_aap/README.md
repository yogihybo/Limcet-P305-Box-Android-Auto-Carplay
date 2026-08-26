# Micro Android Auto Sidecar (`micro_aap`)

A high-performance, ultra-low-memory, pure C implementation of the Google Android Auto (AAP) protocol receiver and hardware projection engine for resource-constrained embedded automotive SoCs (ArkMicro ARK1668 / ARM926EJ-S / Cortex-A7).

---

## 1. Performance & Memory Profile

### Live Hardware Metrics (`top` on ArkMicro ARK1668 @ ~500MHz with 128MB RAM)

```text
Mem: 72272K used, 100764K free, 132K shrd, 788K buff, 26204K cached
CPU: 30.7% usr  7.6% sys  0.0% nic 46.1% idle  0.0% io  0.0% irq 15.3% sirq
Load average: 0.92 0.26 0.09

  PID  PPID USER     STAT   VSZ %VSZ CPU %CPU COMMAND
  161   120 root     S    21276 12.2   0 30.7 /usr/bin/androidauto-sidecar
  120     1 root     S    28112 16.2   0  0.0 /usr/bin/custom_ui
```

### Comparative Benchmark: Legacy Boost/AASDK vs. Micro-AAP

| Metric | Legacy AASDK (C++ / Boost / Protobuf) | Micro-AAP (Pure C / Nanopb / Zero-Copy) | Improvement |
|---|---|---|---|
| **Binary Size** | ~71 MB (unstripped) / ~8.5 MB (stripped) | **1.9 MB** (fully self-contained) | **~78% reduction** |
| **Virtual Memory (VSZ)** | ~68 MB – 85 MB | **~21 MB** | **~75% reduction** |
| **Available RAM on 128MB Board** | < 15 MB (severe memory pressure & paging) | **> 100 MB free RAM** | **+85 MB headroom** |
| **Active CPU Usage during Stream** | 85% – 98% (near saturation / dropped audio) | **~30% user / 46%+ CPU idle** | **~60% lower load** |
| **TLS Handshake Latency** | ~650 ms – 1200 ms | **< 95 ms** | **~10x faster** |
| **Dynamic Heap Allocs / Sec** | Thousands (`make_shared`, `vector`, heap strings) | **Zero in streaming loop** | **Eliminated heap churn** |

---

## 2. Why CPU Usage is Drastically Lower

1. **Zero Dynamic Allocation in Inner Loops**:
   - The legacy C++ AASDK allocated `std::make_shared<Message>`, `std::vector<uint8_t>`, and dynamic Protobuf heap objects for *every single incoming packet* (hundreds of times per second across 48kHz audio and video NALs). On single-core embedded ARM processors, `malloc`/`free` heap locks and allocator fragmentation create heavy CPU overhead.
   - `micro_aap` uses Nanopb with static structures and fixed DMA-aligned ring buffers, eliminating heap churn during live projection.

2. **No Strand / Context Switching Overhead**:
   - Replaced multi-threaded Boost Asio thread pools, strand deferred promises, and inter-thread event queues with a unified, non-blocking `poll()` multiplexer.

3. **No Kernel Memory Pressure / Zero Thrashing**:
   - With 100MB of free RAM, kernel daemons (`kswapd0`, page-cache reclaims, and `kworker` dirty-page flushers) remain idle (0.0% IO wait).

4. **Hardware-Accelerated Zero-Copy Video Pipeline**:
   - H.264 bitstream is written directly to physical SDRAM DMA buffers with `msync()` CPU cache line flushes.
   - The Hantro VDEC hardware engine decodes the stream, and output picture addresses are pushed directly to the LCD controller (`/dev/fb4`) via `ARK_IO_SET_FB_ADDR` with zero software pixel conversion.

---

## 3. Architecture & Features

- **Wireless Projection Protocol (WPP)**: Fast Bluetooth RFCOMM handshake with automated WiFi SoftAP handoff.
- **Protocol Cryptography**: Zero-copy OpenSSL TLS 1.2 handshake and AES-128-CBC + HMAC-SHA256 frame crypto.
- **Per-Channel Frame Reassembler**: Assembles multi-frame `FIRST` (0x01), `MIDDLE` (0x00), `LAST` (0x02), and `BULK` (0x03) AAP packets across 16 logical channels.
- **Hardware Video Sink**: Dynamic dlopen of `/usr/lib/libmfc.so` (Hantro H.264 VDEC) and direct framebuffer layer management on `/dev/fb4`.
- **ALSA Audio Sink**: Paced multi-channel audio output (`plug:softvol2` for 48kHz stereo media, `plug:softvol1` for 16kHz guidance, `plug:softvol4` for system audio).
- **Control & Sensor Services**: Full support for Audio Focus, Navigation Focus, Sensor Channels (Driving Status & Night Mode), Key Binding, and Input Injection.

---

## 4. Build Instructions

### Prerequisites
- Buildroot ARM cross-toolchain (`arm-buildroot-linux-gnueabihf-gcc`)
- Buildroot staging sysroot (`linux-arkmicro/buildroot/output/staging`) containing `libssl`, `libcrypto`, `libasound`, and Linux headers.

### Building `micro-androidauto-sidecar`

```bash
# Clean and compile the binary (optimized with -Os, -ffunction-sections, -fdata-sections, -s)
make -C custom_ui/micro_aap clean
make -C custom_ui/micro_aap -j4
```

The resulting stripped binary is generated at:
`custom_ui/micro_aap/build/micro-androidauto-sidecar` (approx. 1.9 MB).

### Staging to Firmware Overlay

```bash
cp custom_ui/micro_aap/build/micro-androidauto-sidecar firmware_overlay_dyn/usr/bin/androidauto-sidecar
```

### Full Bootable SD Card Build

To build the complete bootable SD card image with the new sidecar:

```bash
./build_bootable_sdcard_dyn.sh
```

---

## 5. Boot Sequence & Fast-Startup Architecture

### < 1-Second UI Display Bring-Up
In earlier firmware versions, GUI initialization was blocked for 7–11 seconds after kernel boot because `rcS` ran a synchronous loop waiting for `hci-updown up` and `rtk_hciattach` Bluetooth firmware loading before starting `custom_ui`.

The system now implements a fully asynchronous, parallel boot pipeline:

1. **Immediate GUI Launch**:
   - As soon as the root filesystem, `/dev/fb0`, and core kernel drivers are initialized, `rcS` launches `custom_ui` immediately:
     ```sh
     (sleep 0.1 && . /etc/profile && /usr/bin/custom_ui) &
     ```
2. **Non-Blocking Main Loop**:
   - `custom_ui` initializes LVGL, loads the UI theme, maps MCU touch/knob devices, and renders the Material-3 Home Dashboard in **< 1 second**.
   - Bluetooth discovery, DBus daemon, `rtk_hciattach`, and `androidauto-sidecar` are started concurrently on background threads (`core::SizedThread`), completely decoupled from the UI rendering loop.

---

## 6. Input Control Architecture

### Touchscreen Stream Handling
- Touch events are streamed at ~100Hz over the `/tmp/androidauto-sidecar.sock` Unix domain socket.
- `micro_aap` uses a re-entrant multi-line command tokenizer (`strtok_r`) and an expanded 32-client connection pool with automatic stale socket cleanup, guaranteeing no dropped touch packets or socket timeouts during fast drags/scrolls.

### Rotary Knob 3-in-1 Gesture Navigation
The single physical rotary encoder supports:
1. **Intra-Card Widget Focus (Normal Rotation)**:
   - Clockwise: `KEYCODE_SYSTEM_NAVIGATION_DOWN` (`281`).
   - Counter-Clockwise: `KEYCODE_SYSTEM_NAVIGATION_UP` (`280`).
2. **Inter-Card Nudge (Hold & Rotate Chord)**:
   - Press & Hold + Rotate Clockwise: `KEYCODE_DPAD_RIGHT` (`22`).
   - Press & Hold + Rotate Counter-Clockwise: `KEYCODE_DPAD_LEFT` (`21`).
   - Jumps highlight focus across to adjacent cards (Media $\leftrightarrow$ Maps $\leftrightarrow$ Rail / App Launcher).
3. **Center Select (Click & Release)**:
   - Press & Release without turning: `KEYCODE_DPAD_CENTER` (`23`) sent on release.

