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
// RESOLVED (was flagged as a gap, checked via Ghidra): the official
// current API's H264DecInit takes FOUR arguments after the instance
// pointer (noOutputReordering, useVideoFreezeConcealment,
// useDisplaySmoothing, DecDpbFlags dpbFlags), which didn't match our
// earlier decompile of this target recovering only THREE. Re-checked
// directly against this target's real libmfc.so: the exported
// "H264DecInit" symbol is actually a thin stub that jumps through a
// function-pointer slot (PTR_H264DecInit_00083774); decompiling the
// stub alone reports a bogus `(void)` signature. Resolving that
// pointer and decompiling the real implementation at 0x1ba90 confirms
// it genuinely takes exactly THREE arguments after the instance
// pointer -- our existing 3-zero call site below is correct. This
// target ships an older/trimmed API variant (closer to the ~2010-era
// ST-Ericsson dmw96 lineage than VeriSilicon's current 4-arg version):
// param_2/param_4 are forwarded into h264bsdInit(ctx, param_2,
// param_4) (bitstream-layer init flags), param_3 is conditionally
// zeroed when the ASIC ID reads 0x8170 before being stored into the
// decoder context struct -- exact semantic mapping to the official
// parameter names not pinned down, but the call site's argument COUNT
// is now hardware-confirmed correct, not a guess.
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
    // true if a picture became ready (H264DecNextPicture returned
    // H264DEC_PIC_RDY=2 -- see hantro_h264_decoder.cpp's own comment
    // for why this isn't simply "returned 0": 0-6 are all real,
    // non-error H264DecRet values, only negative ones are actual
    // errors), in which case last_picture() reflects the ready frame's
    // real fields (width/height/crop/output buffer address/format).
    bool decodeFrame(const uint8_t * data, size_t len);

    // Valid only after decodeFrame() returns true.
    const H264DecPicture & last_picture() const { return lastPicture_; }

    // 2026-08-17 TRIED AND REVERTED: a real hardware dmesg capture
    // showed the Hantro decoder allocates exactly TWO internal
    // reference-picture buffers via /tmp/dev/memalloc and never a
    // third, which looked like a plausible decoder-vs-display buffer
    // race (no H264DecPictureConsumed()-equivalent API exists in this
    // device's libmfc.so, confirmed absent from its exported symbol
    // table, to tell the decoder "not yet"). A stabilize_output()
    // method briefly lived here, copying each ready picture into a
    // pair of buffers this class owned before exposing their address
    // instead of last_picture().outputPictureBusAddress directly.
    // Removed once real stock's own sink binary was confirmed (via
    // decompile) to push this exact same raw decoder address straight
    // to the display, zero copy, on this identical kernel/hardware,
    // with no corruption -- directly falsifying the buffer-race theory
    // (if it were real, stock would hit it too). The copy was also a
    // real, measurable latency cost stock's own path doesn't pay,
    // plausibly making this side fall further behind during exactly
    // the interaction-driven bursts where residual corruption showed
    // up -- the fix may have been contributing to its own symptom.
    // Callers now push last_picture().outputPictureBusAddress
    // directly, matching stock exactly.

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
