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
//   2026-08-16 CORRECTION: this file previously claimed no other
//   binary references either function. Wrong -- usr/bin/mplayer
//   (found while cross-checking the vsync fix against real stock
//   code, see wait_for_vsync()'s own comment) genuinely calls both,
//   confirming the field mapping above byte-for-byte against a real
//   call site (fd, width, height, out_x, out_y, out_width,
//   out_height, crop_left=0, crop_right=0, crop_top, crop_bottom,
//   format), AND showing crop_top/crop_bottom are NOT always zero in
//   real stock use -- mplayer's caller (FUN_000437b0) passes
//   crop_top=2, crop_bottom=2 in its general-purpose video-scaling
//   path, paired with code that forces the output height even
//   (`if (uVar8 & 1) uVar8 -= 1`) -- i.e. real stock uses nonzero
//   crop to trim an odd row off an odd-height source, not something
//   relevant to AA's own stream (800x480, already even/16-aligned on
//   both axes). This file still passes zero crop on all four for AA
//   specifically -- correct for AA's actual resolution, just no
//   longer justified by "there's no real caller to check against".
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

// 2026-08-16: found on real hardware -- a scaled, correctly-colored,
// non-tiled AA frame finally appeared (previous fixes all confirmed
// working), but with a grey/whitish wash over the true colors. Rather
// than touch this device's own (deliberately simplified, see
// set_frame_addr()'s own comment) kernel driver, this stays userspace-
// only: `struct ark_fb_blend { alpha_blend_en; per_pix_alpha_blend_en;
// blend_mode; alpha; }` via ARKFB_SET_BLEND = _IOW('O', 41,
// struct ark_fb_blend) = 0x40104f29 (real vendor ioctl, confirmed via
// disassembly of stock's own ark_fb_set_blend() -- see
// ark_lcdc_common.h's own comment) is the real, dynamically-callable
// vendor mechanism for exactly this: forces alpha_blend_en=0,
// per_pix_alpha_blend_en=0, blend_mode=0, alpha=0xff (fully opaque,
// no blending) explicitly from here, rather than assuming it's already
// correct as a side effect of the update_window call above (whatever
// this device's own kernel does internally for that ioctl is exactly
// the kind of internal-implementation-detail this whole rewrite is
// trying to stop depending on -- this makes the blend state an
// explicit, direct userspace call instead).
struct ArkFbBlend {
    int32_t alpha_blend_en;
    int32_t per_pix_alpha_blend_en;
    int32_t blend_mode;
    uint8_t alpha;
    uint8_t _pad[3];  // struct is 16 bytes (matches ARKFB_SET_BLEND's
                       // own _IOW size field) -- alpha is a single byte
                       // in the real struct, padded to 4-byte alignment.
};

constexpr unsigned long kArkfbSetBlend = 0x40104f29;

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

// FBIO_WAITFORVSYNC = _IOW('F', 0x20, __u32) -- standard Linux uapi
// (include/uapi/linux/fb.h), NOT one of this device's own ARK_IO()
// vendor numbers. Confirmed genuinely implemented (real IRQ wait, not
// a stub) by this device's own ark1668_lcdc_funcs.c, AND confirmed as
// the exact number libarkcmn.so's own arkapi_wait_for_vsync() calls --
// see wait_for_vsync()'s own doc comment in video_layer.h.
constexpr unsigned long kFbioWaitForVsync = 0x40044620;

// ARK_IO(54), read direction -- real vendor GET ioctl, ported from
// libarkcmn.so's own arkapi_get_fb_addr(). See get_frame_addr()'s own
// doc comment in video_layer.h.
constexpr unsigned long kArkGetFbAddr = 0x80104f36;

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

    // See ArkFbBlend's own comment -- explicit, direct call rather than
    // assuming this is already correct as a side effect of the ioctl
    // above. Non-fatal on failure (logs only) -- a grey/washed tint is
    // a real visible defect but not worth aborting video entirely over.
    ArkFbBlend blend{};
    blend.alpha_blend_en = 0;
    blend.per_pix_alpha_blend_en = 0;
    blend.blend_mode = 0;
    blend.alpha = 0xff;
    if (ioctl(h.fd, kArkfbSetBlend, &blend) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::configure_video_layer: ioctl(ARKFB_SET_BLEND) failed (%s)\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
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

bool wait_for_vsync(VideoLayerHandle & h) {
    if (h.fd < 0) return false;
    uint32_t arg = 0;
    if (ioctl(h.fd, kFbioWaitForVsync, &arg) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::wait_for_vsync: ioctl(FBIO_WAITFORVSYNC) failed (%s)\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

bool get_frame_addr(VideoLayerHandle & h, uint32_t & outYAddr) {
    if (h.fd < 0) return false;
    ArkDispAddr addr{};
    if (ioctl(h.fd, kArkGetFbAddr, &addr) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::get_frame_addr: ioctl(ARK_GET_FB_ADDR) failed (%s)\n",
                     core::log_timestamp().c_str(), std::strerror(errno));
        return false;
    }
    outYAddr = addr.yaddr;
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
