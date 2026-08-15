#include "hal/video_layer.h"
#include "core/log_timing.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hal {

namespace {

// 2026-08-16: both of this file's two previous fix attempts (a
// yuv_order bit-guess, then five direct /dev/mem register writes)
// left real hardware unchanged -- same pink/green tint, same tiled/
// doubled image, both times. Rather than guess a third time, this
// file now ports the REAL vendor logic directly: Ghidra-decompiled
// `arkapi_init_fb_video_display`/`arkapi_set_fb_video_addr` from the
// actual deployed firmware_source/mtd6_rootfs/usr/lib/libarkcmn.so
// (the same library stock's own ArkMediaPlayer/MsnCoreApp calls for
// this exact video layer) instead of continuing to hand-guess the
// ioctl protocol from register offsets. Ground truth, not a
// reconstruction:
//
//   arkapi_set_fb_video_addr(fd, y, cb, cr, wait_vsync) ->
//       ioctl(fd, 0x40104f38, &{y, cb, cr, wait_vsync})
//   -- SAME 16-byte {yaddr,cbaddr,craddr,wait_vsync} struct this file
//   already had (ArkDispAddr below, unchanged), but the REAL command
//   number is 0x40104f38, not 0x40104f2c (_IOW('O',44,...), what this
//   file used until now -- ARK_IO(44) was this project's own
//   RECONSTRUCTED kernel tree's guess, not confirmed against the real
//   deployed vendor userspace, same class of gap already documented
//   for the SHOW/HIDE ioctl numbers in hal/display.cpp). A wrong
//   command number here means every frame address update has been
//   landing on the kernel driver's default/unhandled case this whole
//   time, not the real per-frame address handler.
//
//   arkapi_init_fb_video_display(fd, width, height, out_x, out_y,
//       out_width, out_height, crop_left, crop_right, crop_top,
//       crop_bottom, format) builds a SINGLE real struct
//       ark_disp_update_window (60 bytes, matches
//       ark_lcdc_common.h exactly) and sends it via ONE ioctl,
//       0x403c4f37 -- not the two separate legacy
//       SET_WINDOW_FORMAT(43)/SET_WINDOW_SIZE(42) ioctls this file
//       used until now (whose real kernel-side handling was never
//       actually confirmed -- they're absent from the real ioctl
//       dispatch table this project decompiled in
//       docs/1.7.1_ARK_DISP_STOCK_DECOMPILATION.md, meaning they may
//       never have reached the real video-window config path at all).
//   Field-by-field byte-offset math, confirmed against the decompiled
//   assignment sequence (each local_XX's own stack-offset naming IS
//   its byte offset into the struct, verified arithmetically, not
//   assumed):
//     win_x        = crop_left
//     win_y        = crop_top
//     win_width    = width  - crop_left - crop_right
//     win_height   = height - crop_top  - crop_bottom
//     width        = 16-aligned buffer width
//     height       = 16-aligned buffer height
//     format       = format arg
//     rgb_order    = 0 (NEVER set by this real function -- always the
//                    memset()'d 0, for every call, not conditional on
//                    anything. This directly contradicts this file's
//                    own earlier "yuv_order should be UYVY(1)" fix --
//                    that was an unconfirmed guess against a different,
//                    only-partially-traced code path; this is the real
//                    one, straight from the decompile, and it says 0.)
//     yuyv_order   = 0 (same -- never set)
//     out_x        = out_x arg
//     out_y        = out_y arg
//     out_width    = out_width arg
//     out_height   = out_height arg
//     interlace_out/show_tv = persisted per-device state fields this
//                    project doesn't have (libarkcmn.so's own global
//                    device-state array, offsets +0x1b0/+0x1a0) --
//                    left 0, matching a fresh/never-configured device.
//   No other binary on this device's rootfs references either
//   function (grepped) -- there's no real "known-good caller" to
//   cross-check crop-argument semantics against, so this always
//   passes zero crop on all four (full-frame, no cropping), which
//   makes win_x/win_y/win_width/win_height collapse to (0, 0, width,
//   height) regardless of which of crop_left/crop_right the real
//   semantics actually are -- sidesteps the ambiguity entirely rather
//   than guess it.
//
// Deliberately does NOT dlopen libarkcmn.so itself: this device's
// static-NSS-crash workaround (hantro_dlopen.c) only supports loading
// exactly one hardcoded library (libmfc.so) via a purpose-built
// minimal ELF loader, and libarkcmn.so's own per-fd device-tracking
// state (its own internal FUN_00016884 fd->index array, used for
// OSD/layer alpha-blend coordination between callers) has no bearing
// on what the KERNEL driver needs to see -- the ioctl protocol itself
// is the only thing that matters at the kernel boundary, and that's
// what's ported here, using this file's own plain open() fd rather
// than the library's own arkapi_open_video_fb() (which takes no
// arguments and does nothing except a plain open() of a fixed device
// path plus that same internal bookkeeping).
struct ArkDispUpdateWindow {
    uint32_t win_x, win_y;
    uint32_t win_width, win_height;
    uint32_t width, height;
    uint32_t format;
    uint32_t rgb_order;
    uint32_t yuyv_order;
    uint32_t out_x, out_y;
    uint32_t out_width, out_height;
    uint32_t interlace_out;
    uint32_t show_tv;
};

constexpr unsigned long kArkVideoUpdateWindow = 0x403c4f37;

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

// Real command number, confirmed via decompile -- see this file's own
// top comment. NOT _IOW('O', 44, ArkDispAddr) (0x40104f2c), which is
// what this file used until now.
constexpr unsigned long kArkfbSetWindowAddrReal = 0x40104f38;

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
        std::fprintf(stderr, "%s hal::video_layer::init_video_layer: warning: %s unavailable (%s)\n", core::log_timestamp().c_str(), path,
                     std::strerror(errno));
        return false;
    }
    std::printf("%s hal::video_layer::init_video_layer: %s opened (fd=%d)\n", core::log_timestamp().c_str(), path, out.fd);
    return true;
}

bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height) {
    if (h.fd < 0) return false;

    ArkDispUpdateWindow win{};
    win.win_x = 0;
    win.win_y = 0;
    win.win_width = width;   // no cropping -- see this file's top comment
    win.win_height = height;
    win.width = (width + 0xfu) & ~0xfu;   // 16-aligned buffer stride, matches real function
    win.height = (height + 0xfu) & ~0xfu;
    win.format = kFormatYUv420;
    win.rgb_order = 0;   // real function never sets this -- see top comment
    win.yuyv_order = 0;  // real function never sets this -- see top comment
    win.out_x = 0;
    win.out_y = 0;
    win.out_width = width;
    win.out_height = height;
    win.interlace_out = 0;
    win.show_tv = 0;

    if (ioctl(h.fd, kArkVideoUpdateWindow, &win) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::configure_video_layer: ioctl(ARK_VIDEO_UPDATE_WINDOW) failed (%s)\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
        return false;
    }

    std::printf("%s hal::video_layer::configure_video_layer: %ux%u, format=Y_UV420 (real vendor ioctl "
                "protocol, ported from libarkcmn.so's arkapi_init_fb_video_display)\n",
                core::log_timestamp().c_str(), width, height);
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

    if (ioctl(h.fd, kArkfbSetWindowAddrReal, &addr) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::set_frame_addr: ioctl(ARK_FB_SET_VIDEO_WINDOW_ADDR) failed (%s)\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
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
