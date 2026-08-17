#include "androidauto/hantro_h264_decoder.h"
#include "androidauto/log_timing.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace androidauto {

namespace {

// Struct layouts confirmed via Ghidra decompile of libmfc.so (see
// tools/hx170-test/hx170-test.c's header comment for the full
// provenance) -- copied here, not re-derived.
struct H264DecInput {
    uint32_t pStream;
    uint32_t streamBusAddress;
    uint32_t dataLen;
    uint32_t picId;
};

struct H264DecOutput {
    uint32_t pStrmCurrPos;
    uint32_t strmCurrBusAddress;
    uint32_t dataLeft;
    uint8_t pad[64];
};

struct MemallocParams {
    uint32_t busAddress;
    uint32_t size;
};

// 2026-08-15: found on real hardware -- ioctl(GETBUFFER) failed with
// ENOTTY ("Inappropriate ioctl for device") on every call, both here
// and in tools/hx170-test.c (byte-for-byte the same sequence). Root
// cause, found by checking the real kernel driver source
// (drivers/soc/arkmicro/memalloc.c/.h in the ark1668ed-bsp tree):
// MEMALLOC_IOCXGETBUFFER is defined as `_IOWR(MEMALLOC_IOC_MAGIC, 1,
// MemallocParams*)` -- note the THIRD macro argument is a POINTER
// type, not the struct itself, so the macro encodes
// sizeof(MemallocParams*)=4 into the command number, not
// sizeof(MemallocParams)=8. The driver's own copy_from_user still
// reads a fixed 8 bytes regardless (hardcoded sizeof(mem_params) in
// the C code, not derived from the command's encoded size field), so
// this was never a data-transfer-size bug -- only the kernel's
// switch(cmd) dispatch match, which needs the EXACT encoded value
// including this "wrong" pointer-sized field, since the driver falls
// through to `default: -ENOIOCTLCMD` (surfaced to userspace as
// ENOTTY) otherwise. 0xc0086b01 (size=8) was the "semantically
// correct" but never-actually-matching value; 0xc0046b01 (size=4) is
// what the real header's macro expansion actually produces.
constexpr unsigned long kMemallocIocxGetbuffer = 0xc0046b01;
constexpr unsigned long kMemallocIocxFreebuffer = 0x40046b02;
constexpr const char * kLibPath = "/usr/lib/libmfc.so";
constexpr const char * kMemallocPath = "/tmp/dev/memalloc";

// 2026-08-15: found on real hardware -- H264DecDecode() was returning
// 1, 3, and 4 on every single call, logged as "failed" every time,
// meaning every decoded frame was being silently discarded even
// though the decoder was genuinely working. Root cause: this is the
// real Hantro H264DecRet enum (confirmed against the vendor SDK header,
// buildroot-external/package/hx170dec/software/include/h264decapi.h in
// the ark1668ed-bsp tree) -- 0 through 6 are ALL non-error/informational
// return values (H264DEC_STRM_PROCESSED=1, H264DEC_PIC_RDY=2,
// H264DEC_PIC_DECODED=3 -- what real hardware was returning almost
// every call, meaning a picture WAS ready -- H264DEC_HDRS_RDY=4,
// H264DEC_ADVANCED_TOOLS=5, H264DEC_PENDING_FLUSH=6); only NEGATIVE
// values are real errors. Treating "!= 0" as failure rejected the
// overwhelming majority of genuinely successful decode calls.
constexpr int kH264DecPicRdy = 2;

}  // namespace

HantroH264Decoder::HantroH264Decoder() = default;

HantroH264Decoder::~HantroH264Decoder() {
    close();
}

bool HantroH264Decoder::open() {
    lib_ = dlopen(kLibPath, RTLD_NOW);
    if (!lib_) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: dlopen(%s) failed: %s\n", androidauto::logTimestamp().c_str(), kLibPath,
                     dlerror());
        return false;
    }

    h264DecInit_ = reinterpret_cast<decltype(h264DecInit_)>(dlsym(lib_, "H264DecInit"));
    h264DecDecode_ = reinterpret_cast<decltype(h264DecDecode_)>(dlsym(lib_, "H264DecDecode"));
    h264DecNextPicture_ =
        reinterpret_cast<decltype(h264DecNextPicture_)>(dlsym(lib_, "H264DecNextPicture"));
    h264DecRelease_ = reinterpret_cast<decltype(h264DecRelease_)>(dlsym(lib_, "H264DecRelease"));

    if (!h264DecInit_ || !h264DecDecode_ || !h264DecNextPicture_ || !h264DecRelease_) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: dlsym failed to resolve one or "
                     "more H264Dec* symbols: %s\n", androidauto::logTimestamp().c_str(), dlerror());
        dlclose(lib_);
        lib_ = nullptr;
        return false;
    }

    // 2026-08-17: real stock (usr/bin/sink's MFCH264Decode, Ghidra-
    // decompiled) calls H264DecInit(&inst, 0, 0, 1) -- the third
    // positional argument here was 0, not stock's 1. This device's own
    // H264DecInit forwards param_2/param_4 (the 2nd/4th args) straight
    // into its internal h264bsdInit(ctx, param_2, param_4) bitstream-
    // layer init flags (confirmed via direct decompile of THIS
    // device's H264DecInit earlier this session, not the official SDK
    // docs) -- meaning this one bit genuinely changes what flags the
    // bitstream layer initializes with, not a cosmetic difference.
    int initRet = h264DecInit_(&decoderInst_, 0, 0, 1);
    if (initRet != 0 || decoderInst_ == nullptr) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: H264DecInit failed (ret=%d) -- "
                     "decoder hardware/driver itself did not initialize\n", androidauto::logTimestamp().c_str(), initRet);
        decoderInst_ = nullptr;
        return false;
    }

    std::printf("%s androidauto::HantroH264Decoder: initialized (real hardware, ASIC confirmed)\n", androidauto::logTimestamp().c_str());
    return true;
}

bool HantroH264Decoder::ensureDmaCapacity(size_t size) {
    if (dmaVirt_ && dmaSize_ >= size) return true;

    if (dmaVirt_) {
        munmap(dmaVirt_, dmaSize_);
        ioctl(dmaFd_, kMemallocIocxFreebuffer, &dmaBus_);
        ::close(dmaFd_);
        dmaFd_ = -1;
        dmaVirt_ = nullptr;
    }

    long pagesize = sysconf(_SC_PAGESIZE);
    uint32_t aligned = static_cast<uint32_t>((size + pagesize - 1) & ~(pagesize - 1));

    // 2026-08-17: real hardware showed video corruption persisting
    // completely unchanged even after pointing the display at a
    // buffer only this process writes to (stabilize_output()) --
    // ruling out any decoder-vs-display buffer race and pointing at
    // something upstream of that entirely: the CPU's own view of
    // this memory being stale. Checked the real memalloc.c kernel
    // driver: buffers come from dma_zalloc_coherent(), and its own
    // mmap handler (memalloc_mmap -> phys_mem_access_prot()) only
    // returns an uncached/write-combine mapping if the fd was opened
    // with O_SYNC -- otherwise a valid PFN falls through to a normal
    // CACHED mapping. This process's open() call had no O_SYNC. The
    // Hantro ASIC writes decoded pixels straight to physical DRAM via
    // its own DMA engine, bypassing CPU cache entirely -- a cached
    // CPU-side mapping of that same memory can read stale cache
    // lines instead of what the hardware actually wrote, independent
    // of any buffer-ownership fix, which is exactly why
    // stabilize_output() alone didn't help (it faithfully copies
    // whatever the CPU reads, stale or not). CONFIRMED against real
    // stock, not just kernel-source reasoning: usr/bin/sink's own
    // VideoDecoder::alloc_input_buffer() opens this same device with
    // flags 0x101002 -- bit 0x1000 is exactly O_SYNC.
    dmaFd_ = ::open(kMemallocPath, O_RDWR | O_SYNC);
    if (dmaFd_ < 0) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: open(%s) failed: %s\n", androidauto::logTimestamp().c_str(),
                     kMemallocPath, std::strerror(errno));
        return false;
    }

    MemallocParams params{0, aligned};
    if (ioctl(dmaFd_, kMemallocIocxGetbuffer, &params) < 0) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: ioctl(GETBUFFER) failed: %s\n", androidauto::logTimestamp().c_str(),
                     std::strerror(errno));
        ::close(dmaFd_);
        dmaFd_ = -1;
        return false;
    }
    if (params.busAddress == 0) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: GETBUFFER returned bus address 0\n", androidauto::logTimestamp().c_str());
        ::close(dmaFd_);
        dmaFd_ = -1;
        return false;
    }

    void * virt = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd_,
                       static_cast<off_t>(params.busAddress));
    if (virt == MAP_FAILED) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: mmap failed: %s\n", androidauto::logTimestamp().c_str(),
                     std::strerror(errno));
        ::close(dmaFd_);
        dmaFd_ = -1;
        return false;
    }

    dmaVirt_ = virt;
    dmaBus_ = params.busAddress;
    dmaSize_ = aligned;
    return true;
}

bool HantroH264Decoder::ensureOutputBuffers(size_t size) {
    if (outVirt_[0] && outSize_[0] >= size) return true;

    // Free any previous (undersized) pair first -- same pattern as
    // ensureDmaCapacity(), just doing it for both slots. Not expected
    // to actually happen in practice (AA's stream resolution is fixed
    // for the session), but handled for correctness if it ever does.
    for (int i = 0; i < 2; ++i) {
        if (outVirt_[i]) {
            munmap(outVirt_[i], outSize_[i]);
            ioctl(outFd_[i], kMemallocIocxFreebuffer, &outBus_[i]);
            ::close(outFd_[i]);
            outFd_[i] = -1;
            outVirt_[i] = nullptr;
        }
    }

    long pagesize = sysconf(_SC_PAGESIZE);
    uint32_t aligned = static_cast<uint32_t>((size + pagesize - 1) & ~(pagesize - 1));

    for (int i = 0; i < 2; ++i) {
        // O_SYNC -- see ensureDmaCapacity()'s own comment for why this
        // matters (uncached/write-combine vs. stale-cache-prone
        // mapping, confirmed against stock's own real flags value).
        // Matters here too: the CPU writes this buffer via memcpy, and
        // the LCDC hardware reads it back via its own DMA -- a cached
        // CPU-side mapping risks the write sitting in cache instead of
        // reaching DRAM before the display scans it out, the same
        // class of coherency gap, just the opposite direction (CPU
        // write -> hardware read, instead of hardware write -> CPU
        // read).
        outFd_[i] = ::open(kMemallocPath, O_RDWR | O_SYNC);
        if (outFd_[i] < 0) {
            std::fprintf(stderr, "%s androidauto::HantroH264Decoder: output buffer %d open(%s) failed: %s\n",
                         androidauto::logTimestamp().c_str(), i, kMemallocPath, std::strerror(errno));
            return false;
        }

        MemallocParams params{0, aligned};
        if (ioctl(outFd_[i], kMemallocIocxGetbuffer, &params) < 0 || params.busAddress == 0) {
            std::fprintf(stderr, "%s androidauto::HantroH264Decoder: output buffer %d GETBUFFER failed: %s\n",
                         androidauto::logTimestamp().c_str(), i, std::strerror(errno));
            ::close(outFd_[i]);
            outFd_[i] = -1;
            return false;
        }

        void * virt = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, outFd_[i],
                           static_cast<off_t>(params.busAddress));
        if (virt == MAP_FAILED) {
            std::fprintf(stderr, "%s androidauto::HantroH264Decoder: output buffer %d mmap failed: %s\n",
                         androidauto::logTimestamp().c_str(), i, std::strerror(errno));
            ioctl(outFd_[i], kMemallocIocxFreebuffer, &params.busAddress);
            ::close(outFd_[i]);
            outFd_[i] = -1;
            return false;
        }

        outVirt_[i] = virt;
        outBus_[i] = params.busAddress;
        outSize_[i] = aligned;
    }

    std::printf("%s androidauto::HantroH264Decoder: allocated output shadow buffers (%u bytes each)\n",
                androidauto::logTimestamp().c_str(), aligned);
    return true;
}

uint32_t HantroH264Decoder::stabilize_output() {
    if (!lastPicture_.pOutputPicture) return 0;

    // 2026-08-17: this used to compute frameSize as 16-aligned width *
    // 16-aligned height * 1.5 bytes/pixel (576000 bytes for this
    // stream's 800x480) -- the same formula hal::set_frame_addr()'s
    // own chroma-offset math assumes for where the DISPLAY hardware
    // reads chroma from. That's still correct for the display side.
    // But it undershoots the decoder's own REAL per-frame buffer size
    // by ~15% (99840 bytes) -- confirmed directly from this device's
    // own dmesg, which shows the Hantro decoder allocating exactly
    // 675840 bytes per internal reference buffer for this stream, not
    // 576000. That gap is very likely internal padding (e.g. border
    // padding around each reference frame for motion-compensation
    // search range, a normal feature of hardware H.264 decoders) --
    // but rather than assume that and risk silently truncating a
    // frame whose real internal layout isn't simply "clean 800x480
    // NV12 plus unused trailing padding", copy the full real,
    // device-confirmed size. Reading this many bytes from
    // pOutputPicture is safe/in-bounds regardless of which
    // explanation is right, since it's exactly what the decoder's own
    // allocation guarantees is there. The display side is unaffected
    // by copying more than it reads -- hal::set_frame_addr() still
    // gets pic.picWidth/picHeight separately and only ever reads
    // 576000 bytes' worth via its own stride math.
    constexpr size_t kRealDecoderBufferSize = 675840;
    const size_t computedSize = static_cast<size_t>(lastPicture_.picWidth) *
                                 static_cast<size_t>(lastPicture_.picHeight) * 3 / 2;
    const size_t frameSize = std::max(computedSize, kRealDecoderBufferSize);
    if (!ensureOutputBuffers(frameSize)) return 0;

    const int target = activeOutBuf_ ^ 1;  // the buffer NOT currently pushed to the display
    std::memcpy(outVirt_[target], lastPicture_.pOutputPicture, frameSize);
    activeOutBuf_ = target;
    return outBus_[target];
}

bool HantroH264Decoder::decodeFrame(const uint8_t * data, size_t len) {
    if (!decoderInst_) return false;
    if (!ensureDmaCapacity(len)) return false;

    std::memcpy(dmaVirt_, data, len);

    H264DecInput input{};
    input.pStream = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dmaVirt_));
    input.streamBusAddress = dmaBus_;
    input.dataLen = static_cast<uint32_t>(len);
    input.picId = ++frameCounter_;

    H264DecOutput output{};
    int decodeRet = h264DecDecode_(decoderInst_, &input, &output);
    if (decodeRet < 0) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: H264DecDecode failed, ret=%d\n", androidauto::logTimestamp().c_str(),
                     decodeRet);
        return false;
    }

    // Always worth checking for a ready picture regardless of the
    // specific non-negative decodeRet above (H264DEC_PIC_DECODED=3 is
    // the common case once headers are parsed, but there's no harm in
    // checking after H264DEC_HDRS_RDY=4/H264DEC_STRM_PROCESSED=1/etc.
    // too -- H264DecNextPicture() itself reports H264DEC_OK=0 when
    // nothing is ready yet, which is the normal/expected outcome most
    // calls, not an error).
    H264DecPicture picture{};
    int pictureRet = h264DecNextPicture_(decoderInst_, &picture, 0);
    if (pictureRet != kH264DecPicRdy) {
        if (pictureRet < 0) {
            std::fprintf(stderr, "%s androidauto::HantroH264Decoder: H264DecNextPicture failed, ret=%d\n",
                         androidauto::logTimestamp().c_str(), pictureRet);
        }
        return false;
    }

    lastPicture_ = picture;
    // 2026-08-16: real hardware showed blocky, grid-aligned color
    // corruption on decoded frames -- a different signature than the
    // (now-fixed) grey wash/tearing chased elsewhere, consistent with
    // either genuine H.264 macroblock decode errors or a display-side
    // partial-buffer-update issue. The old per-picture "picture ready"
    // success log (removed per explicit request -- fires ~30x/sec,
    // console-flood) would have answered this immediately, so it's
    // back in a much quieter form: only logs when nbrOfErrMBs is
    // actually nonzero, silent on every normal frame. If this line
    // never fires while the blocky artifact is visible, that rules out
    // decode-level corruption and points squarely at the display side.
    if (picture.nbrOfErrMBs != 0) {
        std::fprintf(stderr, "%s androidauto::HantroH264Decoder: picture picId=%u has %u error "
                     "macroblock(s)\n", androidauto::logTimestamp().c_str(), picture.picId, picture.nbrOfErrMBs);
    }
    // 2026-08-16: removed the old per-picture "picture ready" success
    // log per explicit request -- fires ~30x/sec once video is
    // playing (once per decoded frame), same console-flood reasoning
    // as session.cpp's own ping-log removal. Video display push
    // itself (video_channel.cpp's pushDecodedFrame(), /dev/fb4) is
    // unaffected -- this only removed the routine success print.
    return true;
}

void HantroH264Decoder::close() {
    if (decoderInst_ && h264DecRelease_) {
        h264DecRelease_(decoderInst_);
        decoderInst_ = nullptr;
    }
    if (dmaVirt_) {
        munmap(dmaVirt_, dmaSize_);
        ioctl(dmaFd_, kMemallocIocxFreebuffer, &dmaBus_);
        dmaVirt_ = nullptr;
    }
    if (dmaFd_ >= 0) {
        ::close(dmaFd_);
        dmaFd_ = -1;
    }
    for (int i = 0; i < 2; ++i) {
        if (outVirt_[i]) {
            munmap(outVirt_[i], outSize_[i]);
            ioctl(outFd_[i], kMemallocIocxFreebuffer, &outBus_[i]);
            outVirt_[i] = nullptr;
        }
        if (outFd_[i] >= 0) {
            ::close(outFd_[i]);
            outFd_[i] = -1;
        }
    }
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
}

}  // namespace androidauto
