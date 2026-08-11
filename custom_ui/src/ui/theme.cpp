#include "ui/theme.h"

#include "core/navigation.h"

namespace ui::theme {

namespace {

// LVGL styles are meant to be long-lived and shared across every
// object that uses them, not rebuilt per-widget -- same convention
// home_screen.cpp's TileStyles already established, generalized here
// so every screen shares one copy instead of each screen (or, before
// this file, no screen at all beyond home_screen) building its own.
struct SharedStyles {
    lv_style_t card;
    lv_style_t card_pressed;
    lv_style_t back_btn;
    lv_style_t back_btn_pressed;
    lv_style_t primary_btn;
    lv_style_t primary_btn_pressed;

    SharedStyles() {
        lv_style_init(&card);
        lv_style_set_bg_color(&card, surface());
        lv_style_set_bg_opa(&card, LV_OPA_COVER);
        lv_style_set_border_width(&card, 1);
        lv_style_set_border_color(&card, surface_border());
        lv_style_set_radius(&card, 16);
        lv_style_set_shadow_width(&card, 0);
        lv_style_set_text_color(&card, text_primary());

        lv_style_init(&card_pressed);
        lv_style_set_bg_color(&card_pressed, surface_pressed());
        lv_style_set_border_color(&card_pressed, accent());
        lv_style_set_border_width(&card_pressed, 2);

        // Circular, semi-transparent icon-only button -- the AA-style
        // back chevron every non-home screen now shares, replacing the
        // default theme's rectangular "< Back" text button.
        lv_style_init(&back_btn);
        lv_style_set_bg_color(&back_btn, surface());
        lv_style_set_bg_opa(&back_btn, LV_OPA_70);
        lv_style_set_border_width(&back_btn, 0);
        lv_style_set_radius(&back_btn, LV_RADIUS_CIRCLE);
        lv_style_set_text_color(&back_btn, accent());
        lv_style_set_shadow_width(&back_btn, 0);
        lv_style_set_pad_all(&back_btn, 0);

        lv_style_init(&back_btn_pressed);
        lv_style_set_bg_color(&back_btn_pressed, surface_pressed());
        lv_style_set_bg_opa(&back_btn_pressed, LV_OPA_COVER);

        // Accent-filled call-to-action -- rounded pill, matching the
        // shape language of the cards/back button rather than the
        // default theme's sharper corners.
        lv_style_init(&primary_btn);
        lv_style_set_bg_color(&primary_btn, accent());
        lv_style_set_bg_opa(&primary_btn, LV_OPA_COVER);
        lv_style_set_border_width(&primary_btn, 0);
        lv_style_set_radius(&primary_btn, 12);
        lv_style_set_text_color(&primary_btn, lv_color_hex(0x0a0a12));
        lv_style_set_shadow_width(&primary_btn, 0);
        lv_style_set_pad_hor(&primary_btn, 20);
        lv_style_set_pad_ver(&primary_btn, 10);

        lv_style_init(&primary_btn_pressed);
        lv_style_set_bg_color(&primary_btn_pressed, accent_dim());
    }
};

SharedStyles & styles() {
    static SharedStyles s;
    return s;
}

}  // namespace

void init(lv_display_t * disp) {
    lv_theme_t * theme = lv_theme_default_init(disp, accent(), lv_color_hex(0x3a3a52),
                                                /*dark=*/true, LV_FONT_DEFAULT);
    lv_display_set_theme(disp, theme);
}

lv_obj_t * add_back_button(lv_obj_t * scr, lv_event_cb_t cb) {
    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &styles().back_btn, 0);
    lv_obj_add_style(btn, &styles().back_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_center(icon);

    // Knob-navigable, same as home_screen's launcher tiles -- see
    // hal/knob.h. LVGL auto-removes this from the group on delete.
    lv_group_add_obj(core::navigation::focus_group(), btn);
    return btn;
}

lv_obj_t * add_title(lv_obj_t * scr, const char * text) {
    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_color(title, text_primary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    return title;
}

lv_obj_t * create_screen_with_header(lv_obj_t ** out_scr, const char * title, lv_event_cb_t back_cb) {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, bg(), 0);
    *out_scr = scr;

    add_back_button(scr, back_cb);
    return add_title(scr, title);
}

void style_card(lv_obj_t * obj) {
    lv_obj_add_style(obj, &styles().card, 0);
    lv_obj_add_style(obj, &styles().card_pressed, LV_STATE_PRESSED);
}

void style_primary_button(lv_obj_t * btn) {
    lv_obj_add_style(btn, &styles().primary_btn, 0);
    lv_obj_add_style(btn, &styles().primary_btn_pressed, LV_STATE_PRESSED);
}

void style_section_label(lv_obj_t * label) {
    lv_obj_set_style_text_color(label, accent(), 0);
}

void style_secondary_text(lv_obj_t * label) {
    lv_obj_set_style_text_color(label, text_secondary(), 0);
}

}  // namespace ui::theme
