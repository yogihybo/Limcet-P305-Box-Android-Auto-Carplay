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
//
// Phase 5: also reachable manually from the home screen launcher as a
// "preview" tile (core::navigation::push(create_reverse_camera_screen)),
// independent of core::ReverseGearWatcher, which is implemented
// (src/core/reverse_gear_watcher.{h,cpp}) but not currently
// instantiated anywhere in main.cpp -- so gear-triggered auto show/hide
// does not actually happen yet despite Phase 4 being checked off. Not
// this phase's scope to fix; flagging it here since it's directly
// relevant to why a manual launcher tile is useful right now.
#include "ui/reverse_camera_screen.h"

#include "core/navigation.h"
#include "core/screen_manager.h"
#include "ui/theme.h"

namespace ui {

namespace {

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

}  // namespace

lv_obj_t * create_reverse_camera_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    // Transparent on purpose -- see this file's top comment. An
    // opaque background here would defeat the point even if the
    // layer-blend theory above holds.
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "REVERSE");
    lv_obj_set_style_text_color(label, theme::text_primary(), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_radius(label, 8, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    // Exit affordance -- required now that the home screen's launcher
    // can push this screen manually as a preview (see home_screen.cpp),
    // not just via the (not-yet-wired-into-main.cpp) automatic
    // reverse_gear_watcher path. Semi-transparent so it doesn't
    // opaquely cover more of the hardware video layer than necessary --
    // keeps its own text label (unlike theme::add_back_button's
    // icon-only circle) since "Exit preview" needs to read as more
    // deliberate than a normal back navigation here.
    lv_obj_t * back_btn = lv_button_create(scr);
    lv_obj_set_style_bg_color(back_btn, theme::surface(), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(back_btn, 12, 0);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_obj_set_style_text_color(back_label, theme::text_primary(), 0);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Exit preview");

    return scr;
}

}  // namespace ui
