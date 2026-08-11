// H.264 decode via the Hantro G1 (hx170dec) hardware decoder, driven
// through the target's own dlopen'd libmfc.so -- see the kernel driver
// itself (linux-arkmicro/linux/drivers/soc/arkmicro/hx170dec/hx170dec.c)
// for why this is necessary, not just convenient: the kernel interface
// is a RAW register push/pull/wait ioctl set (HX170DEC_IOCS_DEC_PUSH_REG
// etc.) with zero H.264 bitstream awareness -- all SPS/PPS/slice
// parsing and register computation happens in userspace, inside
// libmfc.so. Reimplementing that from scratch would mean writing a
// real software H.264 bitstream parser; dlopen'ing the vendor's own
// already-correct implementation (same as tools/hx170-test/hx170-test.c
// already proved works -- H264DecInit succeeded on real hardware,
// confirmed real ASIC Product ID 0x6731) is the only sane path.
//
// Struct layouts/API signatures below are copied from
// tools/hx170-test/hx170-test.c, which confirmed them via Ghidra
// decompile of the deployed libmfc.so cross-checked against
// H264DecDecode's own input-validation code -- not re-derived here.
//
// KNOWN GAP, stated plainly: H264DecPicture's actual field layout
// (where the decoded frame's luma/chroma plane bus addresses live) is
// NOT yet reverse-engineered -- hx170-test.c only ever read the call's
// return code, never decoded individual fields (see its own comment).
// decodeFrame() below therefore reports whether a picture became ready
// (H264DecNextPicture's return value), but callers cannot yet retrieve
// the actual decoded pixel data or push it to the display hardware.
// The concrete next step to close this gap: decompile
// H264DecNextPicture's own write pattern into its output struct
// (mirroring how H264DecInput's field order was confirmed via
// H264DecDecode's read pattern) -- a real, scoped task, not a guess to
// take blindly.
//
// NOT YET hardware-tested against real streamed H.264 data (only ever
// exercised against hx170-test's own minimal hand-built SPS+PPS+IDR
// test stream).
#pragma once

#include <cstddef>
#include <cstdint>

namespace androidauto {

class HantroH264Decoder {
public:
    HantroH264Decoder();
    ~HantroH264Decoder();

    HantroH264Decoder(const HantroH264Decoder &) = delete;
    HantroH264Decoder & operator=(const HantroH264Decoder &) = delete;

    // dlopen's libmfc.so and calls H264DecInit. Returns false (logs
    // the reason) on failure -- non-fatal, matches this codebase's
    // general optional-hardware pattern.
    bool open();

    // Copies `data`/`len` into the DMA input buffer (growing it if
    // needed) and calls H264DecDecode, then H264DecNextPicture. Returns
    // true if a picture became ready (H264DecNextPicture returned 0) --
    // see class header comment for why the actual decoded frame data
    // isn't retrievable yet even when this returns true.
    bool decodeFrame(const uint8_t * data, size_t len);

    void close();

private:
    bool ensureDmaCapacity(size_t size);

    void * lib_ = nullptr;
    void * decoderInst_ = nullptr;

    int (*h264DecInit_)(void **, uint32_t, uint32_t, uint32_t) = nullptr;
    int (*h264DecDecode_)(void *, void *, void *) = nullptr;
    int (*h264DecNextPicture_)(void *, void *, uint32_t) = nullptr;
    void (*h264DecRelease_)(void *) = nullptr;

    // /tmp/dev/memalloc DMA buffer, see hx170-test.c's mem_alloc() --
    // replicated here rather than shared, since it's ~15 lines and this
    // avoids a dependency between the two.
    void * dmaVirt_ = nullptr;
    uint32_t dmaBus_ = 0;
    uint32_t dmaSize_ = 0;
    int dmaFd_ = -1;

    uint32_t frameCounter_ = 0;
};

}  // namespace androidauto
