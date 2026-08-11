#include "hal/video_layer.h"

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
        std::fprintf(stderr, "hal::init_video_layer: warning: %s unavailable (%s)\n", path,
                     std::strerror(errno));
        return false;
    }
    std::printf("hal::init_video_layer: %s opened (fd=%d)\n", path, out.fd);
    return true;
}

bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height) {
    if (h.fd < 0) return false;

    unsigned int format_val = kFormatYUv420;  // yuv_order/rgb_order left 0, not applicable here
    if (ioctl(h.fd, kArkfbSetWindowFormat, &format_val) != 0) {
        std::fprintf(stderr, "hal::configure_video_layer: ARKFB_SET_WINDOW_FORMAT failed (%s)\n",
                     std::strerror(errno));
        return false;
    }

    unsigned int size_val = (width & 0xFFFFu) | ((height & 0xFFFFu) << 16);
    if (ioctl(h.fd, kArkfbSetWindowSize, &size_val) != 0) {
        std::fprintf(stderr, "hal::configure_video_layer: ARKFB_SET_WINDOW_SIZE failed (%s)\n",
                     std::strerror(errno));
        return false;
    }

    std::printf("hal::configure_video_layer: %ux%u, format=Y_UV420\n", width, height);
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
        std::fprintf(stderr, "hal::set_frame_addr: ARKFB_SET_WINDOW_ADDR failed (%s)\n",
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool show_video_layer(VideoLayerHandle & h) {
    if (h.fd < 0) return false;
    if (ioctl(h.fd, kArkfbShowWindowReal, 0) != 0) {
        std::fprintf(stderr, "hal::show_video_layer: ioctl(SHOW_WINDOW_REAL) failed (%s)\n",
                     std::strerror(errno));
        return false;
    }
    std::printf("hal::show_video_layer: shown\n");
    return true;
}

bool hide_video_layer(VideoLayerHandle & h) {
    if (h.fd < 0) return false;
    if (ioctl(h.fd, kArkfbHideWindowReal, 0) != 0) {
        std::fprintf(stderr, "hal::hide_video_layer: ioctl(HIDE_WINDOW_REAL) failed (%s)\n",
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
