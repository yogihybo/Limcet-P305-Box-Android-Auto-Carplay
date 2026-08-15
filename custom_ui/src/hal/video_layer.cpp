#include "hal/video_layer.h"
#include "core/log_timing.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {

namespace {

// Mirrors linux-arkmicro's struct ark_disp_addr exactly (see
// video_layer.h's top comment) -- not included directly since that
// kernel tree isn't part of this repo's build, same convention as
// hal/camera.cpp/hal/display_ctrl.cpp's own hand-copied ioctl structs.
struct ArkDispAddr {
    uint32_t yaddr;
    uint32_t cbaddr;
    uint32_t craddr;
    uint32_t wait_vsync;
};

constexpr unsigned long kArkfbSetWindowFormat = _IOW('O', 43, unsigned int);
constexpr unsigned long kArkfbSetWindowSize = _IOW('O', 42, unsigned int);
constexpr unsigned long kArkfbSetWindowAddr = _IOW('O', 44, ArkDispAddr);

// ARK_LCDC_FORMAT_Y_UV420 -- semi-planar Y + interleaved UV, see
// video_layer.h's top comment.
constexpr unsigned int kFormatYUv420 = 0x11;

// 2026-08-15: found on real hardware -- video appeared with a
// psychedelic pink/green tint, the textbook signature of a swapped
// U/V chroma pair feeding a YUV->RGB conversion. Root cause: this
// format_val used to leave yuv_order at 0, which this device's real
// ark_lcdc_common.h (ark1668ed-bsp) defines as
// `enum ark_lcdc_yuv_order { ARK_LCDC_ORDER_VYUY, ARK_LCDC_ORDER_UYVY,
// ARK_LCDC_ORDER_YVYU, ARK_LCDC_ORDER_YUYV };` -- i.e. 0 means V comes
// first in the interleaved chroma pair. HantroH264Decoder's semi-planar
// YUV420 output is confirmed (hantro_h264_decoder.h's own header
// comment, cross-referenced against three independent vendor SDKs) to
// be standard NV12 -- U before V -- so leaving yuv_order at its default
// fed the LCDC's Y2R conversion the chroma bytes in the wrong order.
// ARK_LCDC_ORDER_UYVY (1) is the matching value. Packed per
// video_layer.h's own documented bit layout: format(0-7) |
// yuv_order(16-19)<<16 | rgb_order(24-27)<<24 -- rgb_order isn't
// applicable to this YUV-native layer, left 0.
constexpr unsigned int kYuvOrderUyvy = 1;
constexpr unsigned int kYuvOrderShift = 16;

// Same real vendor SHOW/HIDE numbers already confirmed and used for
// the OSD1/UI layer in hal/display.cpp -- see video_layer.h's top
// comment for why these, not this kernel tree's own ARKFB_SHOW_WINDOW/
// ARKFB_HIDE_WINDOW enum values.
constexpr unsigned long kArkfbShowWindowReal = 0x4f2b;
constexpr unsigned long kArkfbHideWindowReal = 0x4f2c;

}  // namespace

bool init_video_layer(VideoLayerHandle & out, const char * path) {
    out.fd = open(path, O_RDWR);
    if (out.fd < 0) {
        std::fprintf(stderr, "%s hal::video_layer::init_video_layer: warning: %s unavailable (%s)\n", core::log_timestamp().c_str(), path,
                     std::strerror(errno));
        return false;
    }
    std::printf("%s hal::video_layer::init_video_layer: %s opened (fd=%d)\n", core::log_timestamp().c_str(), path, out.fd);
    return true;
}

bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height) {
    if (h.fd < 0) return false;

    unsigned int format_val = kFormatYUv420 | (kYuvOrderUyvy << kYuvOrderShift);
    if (ioctl(h.fd, kArkfbSetWindowFormat, &format_val) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::configure_video_layer: ARKFB_SET_WINDOW_FORMAT failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }

    unsigned int size_val = (width & 0xFFFFu) | ((height & 0xFFFFu) << 16);
    if (ioctl(h.fd, kArkfbSetWindowSize, &size_val) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::configure_video_layer: ARKFB_SET_WINDOW_SIZE failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }

    std::printf("%s hal::video_layer::configure_video_layer: %ux%u, format=Y_UV420\n", core::log_timestamp().c_str(), width, height);
    return true;
}

bool set_frame_addr(VideoLayerHandle & h, uint32_t yBusAddress, uint32_t width, uint32_t height) {
    if (h.fd < 0) return false;

    // Semi-planar NV12-style: one interleaved UV plane, not two
    // separate ones -- cbaddr == craddr, both pointing at the same
    // chroma region. See video_layer.h's top comment for the real
    // caveat on this offset math.
    uint32_t chromaAddr = yBusAddress + (width * height);

    ArkDispAddr addr{};
    addr.yaddr = yBusAddress;
    addr.cbaddr = chromaAddr;
    addr.craddr = chromaAddr;
    addr.wait_vsync = 1;  // wait for vsync before the address takes effect -- avoids tearing

    if (ioctl(h.fd, kArkfbSetWindowAddr, &addr) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::set_frame_addr: ARKFB_SET_WINDOW_ADDR failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool show_video_layer(VideoLayerHandle & h) {
    if (h.fd < 0) return false;
    if (ioctl(h.fd, kArkfbShowWindowReal, 0) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::show_video_layer: ioctl(SHOW_WINDOW_REAL) failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }
    std::printf("%s hal::video_layer::show_video_layer: shown\n", core::log_timestamp().c_str());
    return true;
}

bool hide_video_layer(VideoLayerHandle & h) {
    if (h.fd < 0) return false;
    if (ioctl(h.fd, kArkfbHideWindowReal, 0) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::hide_video_layer: ioctl(HIDE_WINDOW_REAL) failed (%s)\n", core::log_timestamp().c_str(),
                     std::strerror(errno));
        return false;
    }
    return true;
}

void close_video_layer(VideoLayerHandle & h) {
    if (h.fd >= 0) {
        close(h.fd);
        h.fd = -1;
    }
}

}  // namespace hal
