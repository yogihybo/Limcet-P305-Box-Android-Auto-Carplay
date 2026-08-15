// Hardware video-overlay layer control -- pushes a decoded Android
// Auto/CarPlay ("phonelink") frame's plane addresses directly to the
// LCDC compositor, bypassing LVGL/the framebuffer entirely (the same
// "hardware layer does the compositing, our GUI layer just needs to
// get out of the way" pattern already used for the reverse-camera
// preview, see reverse_camera_screen.cpp's top comment -- except this
// layer takes a real address we write, not a raw sensor bypass).
//
// Real, ground-truth source for everything below: this device's own
// kernel driver source
// (linux-arkmicro/linux/drivers/video/fbdev/arkmicro/ark1668e_lcdc_funcs.c
// and ark_lcdc_common.h), NOT a decompile/guess:
//
//   - Device node: ark1668e_lcdc_convert_layer() (ark1668e_lcdc_funcs.c)
//     maps fb device minor numbers to hardware layers, with the
//     driver's OWN comments:
//         fb0 -> OSD2   "for UI"
//         fb1 -> VIDEO2 "for video/carback/phonelink"   <- this one
//         fb2 -> OSD1   "overlay for UI (carback track/radar)"
//         fb3 -> VIDEO1 "tvout"
//         fb4 -> OSD3   "aux for(itu601/itu656)"
//     "phonelink" in the fb1/VIDEO2 comment is this exact feature --
//     Android Auto/CarPlay video. /dev/fb0 is confirmed separately
//     (hal/display.cpp) as this project's own LVGL UI layer (OSD2),
//     consistent with this table.
//   - struct ark_disp_addr { yaddr; cbaddr; craddr; wait_vsync; } and
//     ARKFB_SET_WINDOW_ADDR = _IOW('O', 44, struct ark_disp_addr) --
//     ark_lcdc_common.h, exact field layout the ioctl handler
//     copy_from_user()s into (ark1668e_lcdc_funcs.c).
//   - ARKFB_SET_WINDOW_FORMAT = _IOW('O', 43, unsigned int), packed as
//     format(bits 0-7) | yuv_order(bits 16-19)<<16 | rgb_order(bits
//     24-27)<<24, passed via a pointer to that packed value (the
//     handler copy_from_user()s sizeof(unsigned int), it is NOT passed
//     by ioctl's raw arg value).
//   - ARKFB_SET_WINDOW_SIZE = _IOW('O', 42, unsigned int), packed as
//     width(bits 0-15) | height(bits 16-31), also via a pointer.
//   - ARK_LCDC_FORMAT_Y_UV420 = 0x11 -- semi-planar Y + interleaved UV,
//     matching HantroH264Decoder's confirmed H264DEC_SEMIPLANAR_YUV420
//     output format exactly (see hantro_h264_decoder.h).
//   - Show/hide: uses the SAME real vendor ioctl numbers already
//     confirmed and used for the OSD1/UI layer in hal/display.cpp
//     (0x4f2b show / 0x4f2c hide) rather than this kernel tree's own
//     ARKFB_SHOW_WINDOW/HIDE_WINDOW enum values (ARK_IO(39)/(40)) --
//     see hal/display.cpp's kArkfbShowWindowReal comment and this
//     project's project_hide_window_ioctl_fix memory: the real
//     deployed vendor userspace headers use different numbers than
//     this "reconstructed" kernel tree's own enum, already confirmed
//     once for OSD1; assumed (not yet independently re-confirmed) to
//     be the same fixed numbers regardless of which layer/fd, since
//     the vendor's own ark_disp_fb_ioctl dispatch is one function
//     shared across all layers.
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
// real decoded frame yet.
#pragma once

#include <cstdint>

namespace hal {

struct VideoLayerHandle {
    int fd = -1;
};

// Opens /dev/fb1 (VIDEO2/"phonelink", see top comment). Non-fatal
// pattern, same as every other optional-hardware HAL in this codebase.
bool init_video_layer(VideoLayerHandle & out, const char * path = "/dev/fb1");

// Sets ARK_LCDC_FORMAT_Y_UV420 (semi-planar) and the given frame size
// via ARKFB_SET_WINDOW_FORMAT/ARKFB_SET_WINDOW_SIZE, THEN directly
// writes VIDEO2_SOURCE_SIZE/WIN_SIZE/WIN_POINT/SIZE/POSITION over
// /dev/mem (see video_layer.cpp's own comment) -- real stock sets all
// five of those per frame config, and the two ioctls above only ever
// reach one of them. Call once before the first set_frame_addr() (or
// again if the decoded picture's own dimensions ever change
// mid-session -- not expected in practice for a single AA session, but
// cheap to call again if unsure).
bool configure_video_layer(VideoLayerHandle & h, uint32_t width, uint32_t height);

// Pushes one decoded frame's plane addresses via ARKFB_SET_WINDOW_ADDR.
// `yBusAddress` is HantroH264Decoder's last_picture().outputPictureBusAddress
// directly; this function computes the interleaved-UV chroma address
// as yBusAddress + width*height (see top comment's caveat) and sets
// cbaddr == craddr to it (semi-planar NV12-style: one combined chroma
// plane, not two separate ones -- see ARK_LCDC_FORMAT_Y_UV420).
bool set_frame_addr(VideoLayerHandle & h, uint32_t yBusAddress, uint32_t width, uint32_t height);

// ARKFB_SHOW_WINDOW_REAL / ARKFB_HIDE_WINDOW_REAL (0x4f2b/0x4f2c) --
// same real ioctl already used for the OSD1/UI layer in
// hal/display.cpp, see top comment.
bool show_video_layer(VideoLayerHandle & h);
bool hide_video_layer(VideoLayerHandle & h);

void close_video_layer(VideoLayerHandle & h);

}  // namespace hal
