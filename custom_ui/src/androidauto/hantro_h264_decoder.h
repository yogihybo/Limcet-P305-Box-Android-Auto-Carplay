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
// H264DecPicture's field layout: sourced from real, redistributed
// copies of Hantro/VeriSilicon's own official H.264 decoder host SDK
// (h264decapi.h), found via a GitHub code search and cross-checked
// across THREE independent vendor BSPs spanning different hardware
// generations -- Microchip/Atmel's linux4sam/g1_decoder (SAMA5D4,
// BSD-relicensed redistribution, current), ST-Ericsson's
// Astralix/hardware_drivers dmw96/g1_decoder (~2010-era, missing only
// the later-added picCodingType field), and VeriSilicon's own current
// vpe repo (sdk_inc/VC8000D). All three agree on field order for
// H264DecPicture, and -- importantly -- all three also match our own
// already-decompile-confirmed H264DecInput/H264DecOutput field orders
// exactly (pStream/streamBusAddress/dataLen/picId; pStrmCurrPos/
// strmCurrBusAddress/dataLeft), which is strong corroborating evidence
// this device's libmfc.so is genuinely this same Hantro G1 host SDK,
// not a divergent fork. Not yet independently confirmed against a
// disassembly of THIS target's own H264DecNextPicture, though -- that
// remains the fully conclusive check, just no longer the only lead.
//
// KNOWN, SEPARATE GAP: the official API's H264DecInit takes FOUR
// arguments after the instance pointer (noOutputReordering,
// useVideoFreezeConcealment, useDisplaySmoothing, DecDpbFlags
// dpbFlags) in every version of h264decapi.h found -- our own earlier
// decompile of this target's H264DecInit only recovered THREE. Two
// explanations, unresolved: either this target's libmfc.so genuinely
// exports an older/trimmed 3-arg variant, or the decompile simply
// didn't trace the ARM EABI r3 (4th) argument because nothing inside
// the function used it, in which case open() below is currently
// passing that hardware call whatever garbage happened to sit in r3
// (harmless so far only in the sense that H264DecInit still succeeded
// on real hardware in hx170-test.c -- doesn't rule out a subtler
// effect like an unintended tiled reference-frame format). Flagged,
// not fixed here -- needs a disassembly check of H264DecInit's own
// prologue before changing the call site.
//
// Struct layouts/other API signatures below are otherwise still the
// ones copied from tools/hx170-test/hx170-test.c (confirmed via Ghidra
// decompile of the deployed libmfc.so, cross-checked against
// H264DecDecode's own input-validation code).
//
// Even with the struct now real, decodeFrame() only reports whether a
// picture became ready and exposes its fields via last_picture() --
// actually pushing decoded pixel data to the display hardware (the
// ARKFB_SET_FB_ADDR/SHOW_WINDOW_REAL ioctl path, already confirmed
// working elsewhere in this project) is still a separate, not-yet-done
// step, and NV12/tiled output-format handling on that path is
// unverified.
//
// NOT YET hardware-tested against real streamed H.264 data (only ever
// exercised against hx170-test's own minimal hand-built SPS+PPS+IDR
// test stream).
#pragma once

#include <cstddef>
#include <cstdint>

namespace androidauto {

// Real field layout of Hantro's H264DecPicture -- see this file's
// header comment for provenance (three independent redistributed
// copies of the official h264decapi.h). DecOutFrmFormat/DecPicCodingType
// are plain u32-sized C enums in the original SDK; kept as uint32_t
// here rather than pulling in decapicommon.h.
struct H264DecPicture {
    uint32_t picWidth;
    uint32_t picHeight;
    uint32_t cropLeftOffset;
    uint32_t cropOutWidth;
    uint32_t cropTopOffset;
    uint32_t cropOutHeight;
    const uint32_t * pOutputPicture;
    uint32_t outputPictureBusAddress;
    uint32_t picId;
    uint32_t picCodingType;
    uint32_t isIdrPicture;
    uint32_t nbrOfErrMBs;
    uint32_t interlaced;
    uint32_t fieldPicture;
    uint32_t topField;
    uint32_t viewId;
    uint32_t outputFormat;  // DecOutFrmFormat: 0=raster scan, 1=8x4 tiled
};

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
    // true if a picture became ready (H264DecNextPicture returned 0),
    // in which case last_picture() reflects the ready frame's real
    // fields (width/height/crop/output buffer address/format) -- see
    // class header comment: pushing that buffer to the display is
    // still a separate, not-yet-done step.
    bool decodeFrame(const uint8_t * data, size_t len);

    // Valid only after decodeFrame() returns true.
    const H264DecPicture & last_picture() const { return lastPicture_; }

    void close();

private:
    bool ensureDmaCapacity(size_t size);

    void * lib_ = nullptr;
    void * decoderInst_ = nullptr;

    int (*h264DecInit_)(void **, uint32_t, uint32_t, uint32_t) = nullptr;
    int (*h264DecDecode_)(void *, void *, void *) = nullptr;
    int (*h264DecNextPicture_)(void *, H264DecPicture *, uint32_t) = nullptr;
    void (*h264DecRelease_)(void *) = nullptr;

    // /tmp/dev/memalloc DMA buffer, see hx170-test.c's mem_alloc() --
    // replicated here rather than shared, since it's ~15 lines and this
    // avoids a dependency between the two.
    void * dmaVirt_ = nullptr;
    uint32_t dmaBus_ = 0;
    uint32_t dmaSize_ = 0;
    int dmaFd_ = -1;

    uint32_t frameCounter_ = 0;
    H264DecPicture lastPicture_{};
};

}  // namespace androidauto
