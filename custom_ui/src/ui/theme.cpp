#include "ui/theme.h"

#include "core/navigation.h"
#include "ui/status_bar.h"

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
    lv_style_t icon_badge;
    lv_style_t back_btn;
    lv_style_t back_btn_pressed;
    lv_style_t primary_btn;
    lv_style_t primary_btn_pressed;
    lv_style_t step_btn;
    lv_style_t step_btn_pressed;
    lv_style_t list_btn;
    lv_style_t list_btn_pressed;
    lv_style_t focus_ring;

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

        // Solid accent circle behind a launcher tile's glyph -- the
        // "AA app icon" look (see theme.h's top comment), not a
        // bordered rectangle with a small icon in the corner.
        lv_style_init(&icon_badge);
        lv_style_set_bg_color(&icon_badge, accent());
        lv_style_set_bg_opa(&icon_badge, LV_OPA_COVER);
        lv_style_set_radius(&icon_badge, LV_RADIUS_CIRCLE);
        lv_style_set_border_width(&icon_badge, 0);
        lv_style_set_text_color(&icon_badge, lv_color_hex(0x0a0a12));

        // Circular, semi-transparent icon-only button -- the AA-style
        // back chevron every non-home screen now shares, replacing the
        // default theme's rectangular "< Back" text button. Sized to
        // kMinTouchTarget, not just big enough for the glyph.
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
        // default theme's sharper corners. Generous padding + a bigger
        // font (see style_primary_button()) keeps it well above
        // kMinTouchTarget in height without hardcoding a fixed size,
        // so labels of any length still get a comfortably big target.
        lv_style_init(&primary_btn);
        lv_style_set_bg_color(&primary_btn, accent());
        lv_style_set_bg_opa(&primary_btn, LV_OPA_COVER);
        lv_style_set_border_width(&primary_btn, 0);
        lv_style_set_radius(&primary_btn, 14);
        lv_style_set_text_color(&primary_btn, lv_color_hex(0x0a0a12));
        lv_style_set_shadow_width(&primary_btn, 0);
        lv_style_set_pad_hor(&primary_btn, 28);
        lv_style_set_pad_ver(&primary_btn, 16);
        lv_style_set_min_height(&primary_btn, kMinTouchTarget);

        lv_style_init(&primary_btn_pressed);
        lv_style_set_bg_color(&primary_btn_pressed, accent_dim());

        // One half of a stepper (settings_screen.cpp's add_stepper_row())
        // -- big, square, flat -- outlined rather than filled so a row
        // of label + "-" + value + "+" doesn't turn into a wall of
        // solid accent color.
        lv_style_init(&step_btn);
        lv_style_set_bg_color(&step_btn, surface());
        lv_style_set_bg_opa(&step_btn, LV_OPA_COVER);
        lv_style_set_border_width(&step_btn, 2);
        lv_style_set_border_color(&step_btn, accent());
        lv_style_set_radius(&step_btn, 10);
        lv_style_set_text_color(&step_btn, accent());
        // Smaller than the row's other text (montserrat_24) to fit the
        // +/- glyph inside kStepButtonSize without clipping now that
        // the button itself is half its old size. montserrat_14 is the
        // smallest size this build enables (see lv_conf.h).
        lv_style_set_text_font(&step_btn, &lv_font_montserrat_14);
        lv_style_set_shadow_width(&step_btn, 0);
        lv_style_set_size(&step_btn, kStepButtonSize, kStepButtonSize);

        lv_style_init(&step_btn_pressed);
        lv_style_set_bg_color(&step_btn_pressed, accent());
        lv_style_set_text_color(&step_btn_pressed, lv_color_hex(0x0a0a12));

        // lv_list row (bluetooth_screen.cpp's paired-device list) --
        // the stock lv_list button style is a thin, tightly-padded row;
        // this instead matches the rest of the app's big-target
        // language: kMinTouchTarget tall, generous left padding, no
        // border (list rows are separated by the parent list's own
        // divider lines, not per-row borders).
        lv_style_init(&list_btn);
        lv_style_set_bg_color(&list_btn, surface());
        lv_style_set_bg_opa(&list_btn, LV_OPA_COVER);
        lv_style_set_border_width(&list_btn, 0);
        lv_style_set_radius(&list_btn, 0);
        lv_style_set_text_color(&list_btn, text_primary());
        lv_style_set_text_font(&list_btn, &lv_font_montserrat_20);
        lv_style_set_min_height(&list_btn, kMinTouchTarget);
        lv_style_set_pad_hor(&list_btn, 14);
        lv_style_set_pad_ver(&list_btn, 8);
        // Gap between rows for the knob focus ring (see
        // style_focusable()) -- without it, adjacent rows sat flush
        // against each other with nowhere for the ring to draw,
        // clipping top/bottom. Confirmed on real hardware.
        lv_style_set_margin_ver(&list_btn, 4);

        lv_style_init(&list_btn_pressed);
        lv_style_set_bg_color(&list_btn_pressed, surface_pressed());

        // Knob/encoder focus indicator -- see style_focusable()'s
        // comment. An outline (drawn OUTSIDE the widget's box, unlike
        // border) so it never eats into padding/content or shifts
        // layout, just traces a ring around whatever's currently
        // selected via core::navigation::focus_group().
        lv_style_init(&focus_ring);
        lv_style_set_outline_width(&focus_ring, 3);
        lv_style_set_outline_color(&focus_ring, accent());
        lv_style_set_outline_opa(&focus_ring, LV_OPA_COVER);
        lv_style_set_outline_pad(&focus_ring, 3);
    }
};

SharedStyles & styles() {
    static SharedStyles s;
    return s;
}

}  // namespace

void init(lv_display_t * disp) {
    // Base text size for every default-styled widget's own text (plain
    // lv_label_create() text, switch/dropdown/tabview internals) is
    // montserrat_20 here, not LV_FONT_DEFAULT's 14 -- glanceable body
    // text is as much a part of "simple to operate while driving" as
    // the stepper/button sizing above.
    lv_theme_t * theme = lv_theme_default_init(disp, accent(), lv_color_hex(0x3a3a52),
                                                /*dark=*/true, &lv_font_montserrat_20);
    lv_display_set_theme(disp, theme);
}

lv_obj_t * add_back_button(lv_obj_t * scr, lv_event_cb_t cb) {
    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &styles().back_btn, 0);
    lv_obj_add_style(btn, &styles().back_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &styles().focus_ring, LV_STATE_FOCUSED);
    // Scoped override, smaller than the shared focus_ring style's
    // outline_width(3)/outline_pad(3) -- this button sits close enough
    // to a screen's content below it (e.g. settings_screen.cpp's first
    // row) that the full-size ring was overlapping it. 20% smaller,
    // rounded to the nearest integer pixel (3 * 0.8 = 2.4 -> 2).
    lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_size(btn, kMinTouchTarget, kMinTouchTarget);
    // Status bar (ui/status_bar.h) is pinned to the BOTTOM of the
    // screen now, not the top, so the header doesn't need to dodge it.
    // Moved down 10px (10 -> 20) -- was overlapping the first row of
    // screen content below it (e.g. settings_screen.cpp).
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 20);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    return title;
}

lv_obj_t * create_screen_with_header(lv_obj_t ** out_scr, const char * title, lv_event_cb_t back_cb) {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, bg(), 0);
    *out_scr = scr;

    // Status bar (see ui/status_bar.h -- the persistent AA-style bottom
    // bar every screen but reverse_camera_screen.cpp gets); order
    // relative to the back button/title below doesn't matter, both are
    // absolutely positioned and don't overlap (bar is pinned to the
    // bottom edge).
    status_bar::create(scr);
    add_back_button(scr, back_cb);
    return add_title(scr, title);
}

void style_card(lv_obj_t * obj) {
    lv_obj_add_style(obj, &styles().card, 0);
    lv_obj_add_style(obj, &styles().card_pressed, LV_STATE_PRESSED);
}

void style_icon_badge(lv_obj_t * obj) {
    lv_obj_add_style(obj, &styles().icon_badge, 0);
}

void style_primary_button(lv_obj_t * btn) {
    lv_obj_add_style(btn, &styles().primary_btn, 0);
    lv_obj_add_style(btn, &styles().primary_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &styles().focus_ring, LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_24, 0);
}

void style_step_button(lv_obj_t * btn) {
    lv_obj_add_style(btn, &styles().step_btn, 0);
    lv_obj_add_style(btn, &styles().step_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &styles().focus_ring, LV_STATE_FOCUSED);
}

void style_section_label(lv_obj_t * label) {
    lv_obj_set_style_text_color(label, accent(), 0);
}

void style_secondary_text(lv_obj_t * label) {
    lv_obj_set_style_text_color(label, text_secondary(), 0);
}

void style_list_button(lv_obj_t * btn) {
    lv_obj_add_style(btn, &styles().list_btn, 0);
    lv_obj_add_style(btn, &styles().list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &styles().focus_ring, LV_STATE_FOCUSED);
}

void style_focusable(lv_obj_t * obj) {
    lv_obj_add_style(obj, &styles().focus_ring, LV_STATE_FOCUSED);
}

}  // namespace ui::theme
