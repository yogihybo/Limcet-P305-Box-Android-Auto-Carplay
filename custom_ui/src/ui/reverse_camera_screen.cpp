// The real camera picture, per hal/camera.h's own top comment, is
// composited by the LCDC hardware directly onto its own display layer
// (DISPLAY_LAYER=4 in linux-arkmicro's ark1668_itu656.c) -- it is not
// pixel data this process can read or draw into an LVGL canvas.
// ARK_DVR_GETFRAME is a compiled-in no-op in the driver we're building
// against; there is no frame-capture ioctl to call instead.
//
// So this screen's job is narrower than "show the camera feed": it
// just needs to get out of the way of the hardware layer (transparent
// background) and supply minimal overlay chrome. Whether that
// actually produces "LVGL chrome on top of live video" on real
// hardware depends on how our GUI layer (fb0/OSD1, layer 0 per
// docs/ARCHITECTURE.md's "Display" section) is alpha-blended against
// DISPLAY_LAYER by the LCDC's own multi-layer compositor -- this
// project has prior, hw-confirmed experience with that compositor
// being finicky (see memory project_lcd_alpha_blend_investigation /
// project_stock_ui_rgbmode_mismatch). NOT verified for this specific
// layer pairing on real hardware yet. If it turns out our GUI layer
// simply paints over the video regardless of alpha, the fallback is
// to have the reverse-gear listener fully skip pushing this screen
// (or push it but immediately hide the LVGL display) and let the
// kernel's own app_ready=0 fallback path
// (ark_disp_set_layer_en(0, 0)) hide our whole GUI layer instead --
// see hal::set_app_ready()'s doc comment for that tradeoff.
#include "ui/reverse_camera_screen.h"

#include "core/screen_manager.h"

namespace ui {

lv_obj_t * create_reverse_camera_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    // Transparent on purpose -- see this file's top comment. An
    // opaque background here would defeat the point even if the
    // layer-blend theory above holds.
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "REVERSE");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    return scr;
}

}  // namespace ui
