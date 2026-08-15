#include "hal/video_layer.h"
#include "core/log_timing.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hal {

namespace {

// 2026-08-15: found on real hardware -- video appeared correctly
// colored (after the yuv_order fix) but tiled/doubled, like the
// scaler was reading the wrong source dimensions. Root cause, found by
// reviewing this project's own earlier stock decompile
// (docs/1.7.1_ARK_DISP_STOCK_DECOMPILATION.md): the real stock driver's
// video-window setup path (the internals behind its own
// ark_video_update_window ioctl) writes FIVE separate size/position
// registers per frame config -- VIDEO2_SOURCE_SIZE (0x324, the real
// decoded buffer dimensions the scaler reads FROM), VIDEO2_WIN_SIZE
// (0x32c, how much of that source to use -- full frame here, so same
// as source), VIDEO2_WIN_POINT (0x328, crop start offset -- 0,0, no
// crop), VIDEO2_SIZE (0x330, the LAYER's own output size), and
// VIDEO2_POSITION (0x334, where on the panel). Our ARKFB_SET_WINDOW_SIZE
// ioctl (cmd 42) only ever reached ONE of these -- the other four were
// left at whatever stale value was already in the register (very
// plausibly a stale/zero/leftover value from LCD init), which is
// exactly the kind of mismatch that produces a tiled/repeated image:
// the scaler thinks the source is a different size than what's
// actually being written, so it wraps/repeats reading it.
//
// Decompiled directly from the real setter functions (all five are
// simple, single-purpose, and unambiguous -- unlike the video
// format/yuv_order register, whose real struct-field mapping this
// project could NOT fully pin down from the same decompile, see
// hal/video_layer.cpp's own comment on kFormatYUv420/yuv_order):
//   ark_disp_set_video_source_size(1, w, h)   -> *0x324 = w | h<<0xc
//   ark_disp_set_video_win_size(1, w, h)      -> *0x32c = w | h<<0xc
//   ark_disp_set_video_win_point(1, x, y)     -> *0x328 = x | y<<0xc
//   ark_disp_set_video_layer_size(1, w, h)    -> *0x330 = w | h<<0xc
//   ark_disp_set_video_layer_position(1, x, y)-> *0x334 = x|xneg<<0xc|yneg<<0x19|y<<0xd
//     (layer_position's real packing also encodes sign bits for
//     negative x/y -- not needed here since this is always a
//     full-screen, top-left-anchored layer, so x=y=0 and the sign
//     bits are irrelevant; not reproduced in full).
// All against the real physical base LCD_BASE = 0xE0500000 (same base
// tools/lcdc-regdump already uses, confirmed from U-Boot's own
// ark1668_hardware.h).
constexpr unsigned long kLcdBase = 0xE0500000UL;
constexpr unsigned long kVideo2SourceSizeOffset = 0x324;
constexpr unsigned long kVideo2WinPointOffset = 0x328;
constexpr unsigned long kVideo2WinSizeOffset = 0x32c;
constexpr unsigned long kVideo2LayerSizeOffset = 0x330;
constexpr unsigned long kVideo2PositionOffset = 0x334;

// Lazily mmap()'d once, kept open for the process's lifetime -- same
// "process-lifetime singleton, never freed" convention as every other
// such handle in this codebase. A fresh mmap per call would work too,
// but these registers get rewritten every time the decoded picture's
// dimensions change (rare -- see configure_video_layer()'s own
// caller), so there's no benefit to re-opening /dev/mem repeatedly.
volatile uint8_t * lcd_regs() {
    static volatile uint8_t * regs = [] () -> volatile uint8_t * {
        int fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) {
            std::fprintf(stderr, "%s hal::video_layer: open(/dev/mem) failed for direct LCDC register access: %s\n",
                         core::log_timestamp().c_str(), std::strerror(errno));
            return nullptr;
        }
        void * map = mmap(nullptr, 0x400, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(kLcdBase));
        close(fd);  // mapping itself keeps the region valid, fd not needed after mmap()
        if (map == MAP_FAILED) {
            std::fprintf(stderr, "%s hal::video_layer: mmap(/dev/mem, 0x%lx) failed: %s\n", core::log_timestamp().c_str(), kLcdBase,
                         std::strerror(errno));
            return nullptr;
        }
        return static_cast<volatile uint8_t *>(map);
    }();
    return regs;
}

void write_lcd_reg32(unsigned long offset, uint32_t value) {
    volatile uint8_t * regs = lcd_regs();
    if (!regs) return;
    *reinterpret_cast<volatile uint32_t *>(regs + offset) = value;
}

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

// 2026-08-15 CAVEAT, found reviewing the same stock decompile that led
// to the size-register fix below: the real VIDEO2_CTL register's own
// bit layout (ark_disp_set_video_format, docs/1.7.1_ARK_DISP_STOCK_DECOMPILATION.md)
// does NOT obviously match the format(0-7)|yuv_order(16-19)<<16 layout
// this ioctl's own struct comment (video_layer.h) claims -- that
// function's real packing is param_5<<9|0x400100|rgb_order<<0xe|
// yuv_order<<0x11|..., and which ioctl-struct fields actually supply
// its rgb_order/yuv_order args couldn't be pinned down with confidence
// (the copy sequence feeding it references byte offsets past what the
// struct this project could find accounts for -- likely a larger/
// different struct than assumed). This SET_WINDOW_FORMAT ioctl (cmd
// 42) is a different, simpler path than that one either way, and is
// kept as-is below since it's the one already confirmed to produce a
// real, correctly-dimensioned image on real hardware -- not yet
// independently re-verified whether the yuv_order value below actually
// lands on real hardware as intended, only that it's the best-
// justified value available without guessing blind.
//
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

    // See kVideo2SourceSizeOffset's own comment -- the ioctl above only
    // ever reached one of the five real size/position registers stock
    // sets per frame config. Fill in the other four directly: source =
    // win = the full decoded frame (no cropping), win_point = (0,0)
    // (no crop offset), layer size = the same full frame (no separate
    // scale target), position = (0,0) (top-left/full-screen, matching
    // this layer's existing full-screen assumption elsewhere in this
    // codebase).
    uint32_t packed = (width & 0xFFFu) | ((height & 0xFFFu) << 0xc);
    write_lcd_reg32(kVideo2SourceSizeOffset, packed);
    write_lcd_reg32(kVideo2WinSizeOffset, packed);
    write_lcd_reg32(kVideo2WinPointOffset, 0);
    write_lcd_reg32(kVideo2LayerSizeOffset, packed);
    write_lcd_reg32(kVideo2PositionOffset, 0);

    std::printf("%s hal::video_layer::configure_video_layer: %ux%u, format=Y_UV420, "
                "source/win/layer size + position registers written directly\n",
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
