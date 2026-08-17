// Hardware video-overlay layer control -- pushes a decoded Android
// Auto/CarPlay ("phonelink") frame's plane addresses directly to the
// LCDC compositor, bypassing LVGL/the framebuffer entirely (the same
// "hardware layer does the compositing, our GUI layer just needs to
// get out of the way" pattern already used for the reverse-camera
// preview, see reverse_camera_screen.cpp's top comment -- except this
// layer takes a real address we write, not a raw sensor bypass).
//
// 2026-08-16 MAJOR REVISION: this file used to be built around
// `arkapi_init_fb_video_display`/`arkapi_set_fb_video_addr` -- the
// dedicated hardware video-overlay ioctl pair, confirmed correct
// against libarkcmn.so's own decompile at the time. That protocol
// work wasn't wrong, but the API itself turned out to be wrong: real
// Google Android Auto video (Ghidra-decompiled directly from
// usr/bin/sink's own VideoDecoder class -- `sink` is the actual
// stock GAL/aasdk host process on this device, not a guess or a
// different app like CarLife's msncarlife) never calls either
// function. Its real per-frame path
// (VideoDecoder::draw_slice -> flush_video) uses the GENERIC
// framebuffer API instead -- `arkapi_init_fb_display`/
// `arkapi_set_fb_addr`, the same family used for the regular OSD/UI
// layers. See video_layer.cpp's own top comment for the full
// field-by-field derivation and the real decompiled call sites.
//
//   - Device node: /dev/fb4 (VIDEO_LAYER2) -- CONFIRMED still correct
//     under the new API too: sink's own VideoDecoder::video_init()
//     opens this exact path. (Originally found via a real EINVAL/
//     dmesg trail against /dev/fb1 -- see ark1668_lcdc_funcs.c's
//     `int layer = info->node` / `enum ark1668_lcdc_osdlayer` mapping
//     in git history for that derivation; unchanged by this revision.)
//   - struct ark_disp_addr { yaddr; cbaddr; field3; field4; } (16
//     bytes) sent via ioctl 0x40104f2a (ARK_IO(42)) -- real stock's
//     own caller always passes 0 for field3/field4, not a second
//     chroma address and not a wait_vsync request.
//   - struct ark_disp_update_window (60 bytes, same 15-field layout
//     as before) sent via ioctl 0x403c4f27 (ARK_IO(39)).
//   - ARK_LCDC_FORMAT_Y_UV420 = 0x11 -- semi-planar Y + interleaved UV,
//     matching HantroH264Decoder's confirmed H264DEC_SEMIPLANAR_YUV420
//     output format exactly (see hantro_h264_decoder.h), AND
//     confirmed via sink's own real call (passes format=0x11).
//     Unchanged.
//   - Show/hide: uses the SAME real vendor ioctl numbers already
//     confirmed and used for the OSD1/UI layer in hal/display.cpp
//     (0x4f2b show / 0x4f2c hide). Unchanged by this revision --
//     sink's own arkapi_show_fb() eventually calls the same numbers
//     internally (via arkapi_show_fb_internal(), Ghidra-confirmed).
//
// Per-frame vsync handling: real stock's flush_video() does NOT wait
// for vsync or confirm the previous flip before pushing a new
// address -- it just writes it, every frame, unconditionally
// (wait_for_vsync()/get_frame_addr() below are no longer called from
// the per-frame path for this reason; kept as real, correctly-
// implemented utilities in case tearing reappears and a fallback is
// needed, but real stock's own behavior is the current baseline).
//
// NOT yet hardware-tested. In particular: the chroma-plane offset
// computed in set_frame_addr() (picWidth*picHeight bytes after the Y
// plane start, standard NV12-style semi-planar math) assumes
// HantroH264Decoder's outputPictureBusAddress points at the START of
// a contiguous Y-then-UV buffer with no extra per-plane stride padding
// beyond picWidth itself -- true for this device's confirmed
// VIDEO_800x480 AA video config (both dimensions already 16-pixel-
// aligned), and independently re-confirmed against sink's own
// flush_video() address computation (same formula). Also:
// configure_video_layer()'s crop arguments are always zero (full-
// frame, no cropping) -- CONFIRMED correct for AA's own 800x480
// stream specifically (a real caller, usr/bin/mplayer, only passes
// nonzero crop_top/crop_bottom to trim an odd-height source to even;
// AA's resolution is already even/16-aligned on both axes).
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
// ported directly from libarkcmn.so's own arkapi_init_fb_display
// (see this file's top comment and video_layer.cpp's own comment for
// the full field derivation). Does NOT touch blend/alpha state --
// an explicit ARKFB_SET_BLEND call used to sit here (added to fix a
// grey-wash defect seen under the old, wrong display API) but was
// removed once sink's entire binary was confirmed to never reference
// blend at all; real stock's AA video path leaves this alone. Call
// once before the first set_frame_addr() (or again if the decoded
// picture's own dimensions ever change mid-session -- not expected
// in practice for a single AA session, but cheap to call again if
// unsure).
bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height);

// Pushes one decoded frame's plane addresses via the real
// ark_disp_addr ioctl (0x40104f2a, see this file's top comment).
// `yBusAddress` is HantroH264Decoder's last_picture().outputPictureBusAddress
// directly; this function computes the interleaved-UV chroma address
// as yBusAddress + width*height (see top comment's caveat, and
// video_layer.cpp's comment for the independent re-confirmation
// against sink's own flush_video()). Real stock calls this
// unconditionally, every frame, with no vsync wait/confirm around it
// -- see wait_for_vsync()'s own comment for why this file no longer
// does either on the per-frame path.
bool set_frame_addr(VideoLayerHandle & h, uint32_t yBusAddress, uint32_t width, uint32_t height);

// Blocks until the next real vsync IRQ fires on this layer's LCDC
// controller (standard Linux FBIO_WAITFORVSYNC, confirmed genuinely
// IRQ-backed on this device's own kernel driver -- see
// ark1668_lcdc_funcs.c's ark1668_lcdc_wait_for_vsync()) -- also
// confirmed to be the exact ioctl libarkcmn.so's own exported
// arkapi_wait_for_vsync(fd) calls.
//
// 2026-08-16: NOT called from pushDecodedFrame()'s per-frame path.
// Two earlier fix attempts built real, per-frame vsync handling
// around this function (first a blind wait-then-push, then a
// get_frame_addr()-confirm loop) to chase a blocky-corruption
// artifact -- both were grounded in genuinely real decompiled code,
// but from the wrong app (CarLife's msncarlife, not real Android
// Auto). Decompiling usr/bin/sink's own VideoDecoder::flush_video()
// (the actual stock AA video-push function) shows it does neither:
// it writes the address unconditionally, every frame, no wait, no
// confirm. Kept here as a real, correctly-implemented utility in
// case tearing reappears and a fallback is genuinely needed, but is
// not part of the current per-frame call path.
bool wait_for_vsync(VideoLayerHandle & h);

// Reads back the Y-plane address the LCDC hardware is CURRENTLY
// displaying (not what was last requested) via the real vendor
// ARK_IO(54) GET ioctl, 0x80104f36 -- ported from libarkcmn.so's own
// arkapi_get_fb_addr(). See wait_for_vsync()'s own comment -- not
// called from the per-frame path for the same reason.
bool get_frame_addr(VideoLayerHandle & h, uint32_t & outYAddr);

// ARKFB_SHOW_WINDOW_REAL / ARKFB_HIDE_WINDOW_REAL (0x4f2b/0x4f2c) --
// same real ioctl already used for the OSD1/UI layer in
// hal/display.cpp, see top comment.
bool show_video_layer(VideoLayerHandle & h);
bool hide_video_layer(VideoLayerHandle & h);

void close_video_layer(VideoLayerHandle & h);

}  // namespace hal
