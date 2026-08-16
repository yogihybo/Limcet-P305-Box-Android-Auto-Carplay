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

// 2026-08-16 MAJOR REVISION: everything in this file used to be built
// around `arkapi_init_fb_video_display`/`arkapi_set_fb_video_addr` --
// the dedicated hardware video-overlay ioctl pair (confirmed correct
// against libarkcmn.so's own decompile, see git history). That
// protocol-level work wasn't wrong, but it turned out to be the WRONG
// API: real Google Android Auto video (Ghidra-decompiled directly
// from usr/bin/sink's own VideoDecoder class -- `sink` is the actual
// stock GAL/aasdk host process, not a guess) never calls either of
// those functions at all. Its real per-frame path
// (VideoDecoder::draw_slice -> flush_video) uses the GENERIC,
// non-video framebuffer API instead -- the same family used for the
// regular OSD/UI layers elsewhere in this codebase:
//
//   arkapi_init_fb_display(fd, width, height, out_x, out_y,
//       out_width, out_height, crop_left, crop_right, crop_top,
//       crop_bottom, format) -> ioctl(fd, 0x403c4f27, &window)
//   -- SAME 60-byte ark_disp_update_window struct/field layout as the
//   video variant (confirmed structurally identical via decompile of
//   arkapi_init_fb_display_internal, the real function both the
//   video and non-video wrappers funnel into with the same
//   win_x/win_y/win_width/win_height/width/height/format/rgb_order/
//   yuyv_order/out_x/out_y/out_width/out_height/interlace_out/show_tv
//   layout) -- but a DIFFERENT ioctl command number, ARK_IO(39)
//   (0x403c4f27) instead of ARK_IO(55) (0x403c4f37). Real stock's
//   own caller (sink's VideoDecoder::video_init(), via the dlsym'd
//   "arkapi_init_fb_display" function pointer against /dev/fb4 --
//   same device node this file already used) passes format=0x11
//   (matches kFormatYUv420 below) and 0 for interlace_out/show_tv,
//   confirming those fields too.
//
//   arkapi_set_fb_addr(fd, y, cb_cr, 0, 0) ->
//       ioctl(fd, 0x40104f2a, &{y, cb_cr, 0, 0})
//   -- SAME 16-byte 4-field struct shape as before (ArkDispAddr
//   below, unchanged), but ARK_IO(42) (0x40104f2a) instead of
//   ARK_IO(56) (0x40104f38), and critically: real stock's own caller
//   (sink's VideoDecoder::flush_video(), Ghidra-decompiled) passes
//   the LAST TWO struct fields as literal 0 on every single call --
//   not a wait_vsync=1 request, and no vsync-confirm-loop around it
//   either (see push_frame_addr()'s own history in git log for the
//   two vsync-based fix attempts this revision replaces -- both were
//   grounded in real decompiled code, just the wrong app: CarLife's
//   msncarlife, not real Android Auto's sink). Real stock just writes
//   the address, every frame, unconditionally, and lets the panel
//   pick it up on its own schedule.
//
// Chroma offset math (yBusAddress + width*height for the combined
// Cb/Cr plane) is unchanged and was independently re-confirmed
// against flush_video()'s own real address computation
// (`yAddr + alignedWidth*alignedHeight`).
//
// Field-by-field derivation for the update-window struct otherwise
// carries over unchanged from the video-overlay variant (both
// wrappers funnel into structurally identical internal calls):
//     win_x        = crop_left
//     win_y        = crop_top
//     win_width    = width  - crop_left - crop_right
//     win_height   = height - crop_top  - crop_bottom
//     width        = 16-aligned buffer width
//     height       = 16-aligned buffer height
//     format       = format arg (0x11 confirmed via sink's own call)
//     rgb_order    = 0 (never set by the real function)
//     yuyv_order   = 0 (same -- never set)
//     out_x        = out_x arg
//     out_y        = out_y arg
//     out_width    = out_width arg
//     out_height   = out_height arg
//     interlace_out/show_tv = 0 (confirmed via sink's own call, which
//                    passes these as its own trailing 0, 0 args)
//   Zero crop on all four remains correct for AA's own 800x480
//   stream (already even/16-aligned) -- see git history for the
//   mplayer cross-check that found nonzero crop elsewhere, unrelated
//   to AA's own resolution.
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

// ARK_IO(39) -- real command for arkapi_init_fb_display (the generic
// framebuffer variant real stock AA actually uses). NOT 0x403c4f37
// (ARK_IO(55), arkapi_init_fb_video_display's command), which this
// file used until this revision -- see top comment.
constexpr unsigned long kArkInitFbDisplay = 0x403c4f27;

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
    // Real stock (sink's VideoDecoder::flush_video()) always passes 0
    // for these last two fields on the AA video path -- not a second
    // chroma plane address and not wait_vsync. Named generically
    // since their real semantics were never exercised as anything
    // other than 0 in the one confirmed real call site.
    uint32_t field3;
    uint32_t field4;
};

// ARK_IO(42) -- real command for arkapi_set_fb_addr (the generic
// variant real stock AA actually uses, confirmed via decompile of
// sink's VideoDecoder::flush_video()). NOT 0x40104f38 (ARK_IO(56),
// arkapi_set_fb_video_addr's command), which this file used until
// this revision -- see top comment.
constexpr unsigned long kArkSetFbAddr = 0x40104f2a;

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

    if (ioctl(h.fd, kArkInitFbDisplay, &win) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::configure_video_layer: ioctl(ARK_INIT_FB_DISPLAY) failed (%s)\n",
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
                "protocol, ported from libarkcmn.so's arkapi_init_fb_display)\n",
                core::log_timestamp().c_str(), width, height);
    return true;
}

bool set_frame_addr(VideoLayerHandle & h, uint32_t yBusAddress, uint32_t width, uint32_t height) {
    if (h.fd < 0) return false;

    // Semi-planar NV12-style: one interleaved UV plane immediately
    // after the Y plane. See this file's top comment for the real
    // caveat on this offset math (independently re-confirmed against
    // sink's own flush_video() address computation).
    uint32_t chromaAddr = yBusAddress + (width * height);

    ArkDispAddr addr{};
    addr.yaddr = yBusAddress;
    addr.cbaddr = chromaAddr;
    addr.field3 = 0;  // real stock always passes 0 here -- see struct comment
    addr.field4 = 0;

    if (ioctl(h.fd, kArkSetFbAddr, &addr) != 0) {
        std::fprintf(stderr, "%s hal::video_layer::set_frame_addr: ioctl(ARK_SET_FB_ADDR) failed (%s)\n",
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
