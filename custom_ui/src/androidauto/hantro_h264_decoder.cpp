#include "androidauto/hantro_h264_decoder.h"

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

struct H264DecPicture {
    // See this file's header comment -- layout not yet reverse-
    // engineered, oversized padding so writes from libmfc.so don't
    // overrun this buffer.
    uint8_t pad[256];
};

struct MemallocParams {
    uint32_t busAddress;
    uint32_t size;
};

constexpr unsigned long kMemallocIocxGetbuffer = 0xc0086b01;
constexpr unsigned long kMemallocIocxFreebuffer = 0x40046b02;
constexpr const char * kLibPath = "/usr/lib/libmfc.so";
constexpr const char * kMemallocPath = "/tmp/dev/memalloc";

}  // namespace

HantroH264Decoder::HantroH264Decoder() = default;

HantroH264Decoder::~HantroH264Decoder() {
    close();
}

bool HantroH264Decoder::open() {
    lib_ = dlopen(kLibPath, RTLD_NOW);
    if (!lib_) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: dlopen(%s) failed: %s\n", kLibPath,
                     dlerror());
        return false;
    }

    h264DecInit_ = reinterpret_cast<decltype(h264DecInit_)>(dlsym(lib_, "H264DecInit"));
    h264DecDecode_ = reinterpret_cast<decltype(h264DecDecode_)>(dlsym(lib_, "H264DecDecode"));
    h264DecNextPicture_ =
        reinterpret_cast<decltype(h264DecNextPicture_)>(dlsym(lib_, "H264DecNextPicture"));
    h264DecRelease_ = reinterpret_cast<decltype(h264DecRelease_)>(dlsym(lib_, "H264DecRelease"));

    if (!h264DecInit_ || !h264DecDecode_ || !h264DecNextPicture_ || !h264DecRelease_) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: dlsym failed to resolve one or "
                     "more H264Dec* symbols: %s\n", dlerror());
        dlclose(lib_);
        lib_ = nullptr;
        return false;
    }

    int initRet = h264DecInit_(&decoderInst_, 0, 0, 0);
    if (initRet != 0 || decoderInst_ == nullptr) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: H264DecInit failed (ret=%d) -- "
                     "decoder hardware/driver itself did not initialize\n", initRet);
        decoderInst_ = nullptr;
        return false;
    }

    std::printf("androidauto::HantroH264Decoder: initialized (real hardware, ASIC confirmed)\n");
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

    dmaFd_ = ::open(kMemallocPath, O_RDWR);
    if (dmaFd_ < 0) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: open(%s) failed: %s\n",
                     kMemallocPath, std::strerror(errno));
        return false;
    }

    MemallocParams params{0, aligned};
    if (ioctl(dmaFd_, kMemallocIocxGetbuffer, &params) < 0) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: ioctl(GETBUFFER) failed: %s\n",
                     std::strerror(errno));
        ::close(dmaFd_);
        dmaFd_ = -1;
        return false;
    }
    if (params.busAddress == 0) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: GETBUFFER returned bus address 0\n");
        ::close(dmaFd_);
        dmaFd_ = -1;
        return false;
    }

    void * virt = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd_,
                       static_cast<off_t>(params.busAddress));
    if (virt == MAP_FAILED) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: mmap failed: %s\n",
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
    if (decodeRet != 0) {
        std::fprintf(stderr, "androidauto::HantroH264Decoder: H264DecDecode failed, ret=%d\n",
                     decodeRet);
        return false;
    }

    H264DecPicture picture{};
    int pictureRet = h264DecNextPicture_(decoderInst_, &picture, 0);
    if (pictureRet != 0) {
        // Not necessarily an error -- H264DecNextPicture legitimately
        // returns non-zero when no picture is ready yet (e.g. still
        // buffering reference frames). See class header comment: even
        // when this DOES return 0 (picture ready), the actual pixel
        // data isn't retrievable yet.
        return false;
    }

    std::printf("androidauto::HantroH264Decoder: picture ready (picId=%u) -- TODO: "
               "H264DecPicture plane-address layout not yet reverse-engineered, "
               "can't push this to the display hardware layer yet\n",
               frameCounter_);
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
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
}

}  // namespace androidauto
