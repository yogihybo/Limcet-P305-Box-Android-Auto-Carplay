// Hardware video-overlay layer control -- pushes a decoded Android
// Auto/CarPlay ("phonelink") frame's plane addresses directly to the
// LCDC compositor, bypassing LVGL/the framebuffer entirely (the same
// "hardware layer does the compositing, our GUI layer just needs to
// get out of the way" pattern already used for the reverse-camera
// preview, see reverse_camera_screen.cpp's top comment -- except this
// layer takes a real address we write, not a raw sensor bypass).
//
// 2026-08-16 REVISED: this file's ioctl protocol used to be
// reconstructed from this device's own (separately-maintained)
// "reconstructed" kernel tree source
// (linux-arkmicro/.../ark1668e_lcdc_funcs.c and ark_lcdc_common.h) --
// two real-hardware attempts built on that reconstruction (a yuv_order
// bit guess, then direct /dev/mem register writes) both left the video
// image unchanged (still wrong-tinted, still tiled). Replaced with the
// REAL protocol instead, Ghidra-decompiled directly from this exact
// device's own deployed vendor library,
// firmware_source/mtd6_rootfs/usr/lib/libarkcmn.so's
// arkapi_init_fb_video_display()/arkapi_set_fb_video_addr() -- the
// same functions stock's own ArkMediaPlayer/MsnCoreApp calls for this
// exact layer. See video_layer.cpp's own top comment for the full
// field-by-field derivation.
//
//   - Device node: 2026-08-16 FOUND WRONG -- real hardware rejected
//     every ARKFB_SET_VIDEO_ADDR_RAW call against /dev/fb1 with EINVAL,
//     kernel dmesg logging "ARKFB_SET_VIDEO_ADDR_RAW on non-video
//     layer" (ark1668_lcdc_funcs.c). That message's own driver
//     function name confirmed THIS device runs "ark1668_lcdc" (no
//     trailing E), not the "ark1668e_lcdc" variant this file's fb-to-
//     layer table (fb1 -> VIDEO2) was previously sourced from -- a
//     different driver file with a different mapping. The real,
//     confirmed-from-THIS-driver's-own-source mapping
//     (ark1668_lcdc_funcs.c: `int layer = info->node;` -- literally the
//     fb minor number -- checked against
//     `enum ark1668_lcdc_osdlayer { OSD_LAYER1, OSD_LAYER2, OSD_LAYER3,
//     OSD_LAYER_MAX }` from ark1668_lcdc.h, i.e. OSD_LAYER3=2,
//     OSD_LAYER_MAX=3): fb0/fb1/fb2 are ALL OSD (non-video) layers;
//     only layer > OSD_LAYER3 counts as video, via
//     `vlayer = layer - OSD_LAYER_MAX` -- fb3 -> VIDEO_LAYER1 (vlayer=0),
//     fb4 -> VIDEO_LAYER2 (vlayer=1). This file now opens /dev/fb4
//     (VIDEO_LAYER2) -- matches the "VIDEO2_*" LCDC register block
//     (offset 0x320+, distinct from an earlier, separate "VIDEO_*"
//     block near 0x38 for VIDEO_LAYER1) this project's own earlier
//     register-level investigation already targeted, before that
//     approach was superseded by this file's current ioctl-based one.
//   - struct ark_disp_addr { yaddr; cbaddr; craddr; wait_vsync; } is
//     unchanged (16 bytes, confirmed correct by the decompile too) --
//     but the real command number is 0x40104f38, NOT
//     _IOW('O', 44, struct ark_disp_addr) (0x40104f2c), which is what
//     this file used until now.
//   - struct ark_disp_update_window (60 bytes, win_x/win_y/win_width/
//     win_height/width/height/format/rgb_order/yuyv_order/out_x/out_y/
//     out_width/out_height/interlace_out/show_tv) sent via ONE ioctl,
//     0x403c4f37 -- replaces the old, unconfirmed two-ioctl
//     SET_WINDOW_FORMAT(43)/SET_WINDOW_SIZE(42) pair (absent from the
//     real ioctl dispatch table this project separately decompiled in
//     docs/1.7.1_ARK_DISP_STOCK_DECOMPILATION.md -- they may never
//     have reached the real config path at all).
//   - ARK_LCDC_FORMAT_Y_UV420 = 0x11 -- semi-planar Y + interleaved UV,
//     matching HantroH264Decoder's confirmed H264DEC_SEMIPLANAR_YUV420
//     output format exactly (see hantro_h264_decoder.h). Unchanged.
//   - Show/hide: uses the SAME real vendor ioctl numbers already
//     confirmed and used for the OSD1/UI layer in hal/display.cpp
//     (0x4f2b show / 0x4f2c hide) rather than the reconstructed kernel
//     tree's own ARKFB_SHOW_WINDOW/HIDE_WINDOW enum values (ARK_IO(39)/
//     (40)) -- see hal/display.cpp's kArkfbShowWindowReal comment and
//     this project's project_hide_window_ioctl_fix memory: the real
//     deployed vendor userspace uses different numbers than the
//     reconstructed kernel tree's own enum, already confirmed once for
//     OSD1; assumed (not yet independently re-confirmed) to be the
//     same fixed numbers regardless of which layer/fd, since the
//     vendor's own ark_disp_fb_ioctl dispatch is one function shared
//     across all layers.
//
// NOT yet hardware-tested. In particular: the chroma-plane offset
// computed in set_frame_addr() (picWidth*picHeight bytes after the Y
// plane start, standard NV12-style semi-planar math) assumes
// HantroH264Decoder's outputPictureBusAddress points at the START of
// a contiguous Y-then-UV buffer with no extra per-plane stride padding
// beyond picWidth itself -- true for this device's confirmed
// VIDEO_800x480 AA video config (both dimensions already 16-pixel-
// aligned, so no macroblock padding difference between picWidth and
// the real display width), but not independently verified against a
// real decoded frame yet. Also: configure_video_layer()'s crop
// arguments are always zero (full-frame, no cropping) -- no real
// caller of arkapi_init_fb_video_display exists anywhere else on this
// device's rootfs to confirm crop-parameter semantics against, but
// zero sidesteps the ambiguity regardless of what they mean.
#pragma once

#include <cstdint>

namespace hal {

struct VideoLayerHandle {
    int fd = -1;
};

// Opens /dev/fb4 (VIDEO_LAYER2, see top comment -- NOT /dev/fb1, which
// real hardware confirmed is an OSD/non-video layer on this device's
// real kernel driver). Non-fatal pattern, same as every other
// optional-hardware HAL in this codebase.
bool init_video_layer(VideoLayerHandle & out, const char * path = "/dev/fb4");

// Sets ARK_LCDC_FORMAT_Y_UV420 (semi-planar) and the given frame's
// size/position via a single real ark_disp_update_window ioctl,
// ported directly from libarkcmn.so's own arkapi_init_fb_video_display
// (see this file's top comment and video_layer.cpp's own comment for
// the full field derivation), THEN explicitly forces this layer to
// fully-opaque/no-blend via the real ARKFB_SET_BLEND ioctl (see
// video_layer.cpp's own ArkFbBlend comment for why this isn't just
// assumed as a side effect of the ioctl above). Call once before the
// first set_frame_addr() (or again if the decoded picture's own
// dimensions ever change mid-session -- not expected in practice for
// a single AA session, but cheap to call again if unsure).
bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height);

// Pushes one decoded frame's plane addresses via the real
// ark_disp_addr ioctl (0x40104f38, see this file's top comment).
// `yBusAddress` is HantroH264Decoder's last_picture().outputPictureBusAddress
// directly; this function computes the interleaved-UV chroma address
// as yBusAddress + width*height (see top comment's caveat) and sets
// cbaddr == craddr to it (semi-planar NV12-style: one combined chroma
// plane, not two separate ones -- see ARK_LCDC_FORMAT_Y_UV420).
bool set_frame_addr(VideoLayerHandle & h, uint32_t yBusAddress, uint32_t width, uint32_t height);

// 2026-08-16: real hardware still showed blocky, grid-aligned
// corruption after the blend/protocol fixes (colors now fully
// correct) even with pushDecodedFrame()'s software 16ms throttle in
// place -- expected, since that throttle only rate-LIMITS pushes, it
// doesn't actually know where the panel's scanout beam is, so a push
// can still land mid-refresh and the drift between our timer and the
// panel's real refresh period isn't bounded. Checked this device's
// own kernel driver source (ark1668_lcdc_funcs.c) rather than guess
// again: it genuinely implements the standard Linux FBIO_WAITFORVSYNC
// ioctl with a real IRQ-backed wait_event_interruptible_timeout() on
// a vsync flag set by the actual vsync IRQ handler (see
// ark1668_lcdc_wait_for_vsync(), not a stub/no-op) -- this is the real
// synchronization primitive the "wait_vsync" field in
// set_frame_addr()'s own ioctl was documented as NOT actually
// providing. Blocks the caller until the next real vsync fires.
bool wait_for_vsync(VideoLayerHandle & h);

// ARKFB_SHOW_WINDOW_REAL / ARKFB_HIDE_WINDOW_REAL (0x4f2b/0x4f2c) --
// same real ioctl already used for the OSD1/UI layer in
// hal/display.cpp, see top comment.
bool show_video_layer(VideoLayerHandle & h);
bool hide_video_layer(VideoLayerHandle & h);

void close_video_layer(VideoLayerHandle & h);

}  // namespace hal
