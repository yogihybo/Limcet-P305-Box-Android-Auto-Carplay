#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"

namespace staging_ui::theme {

void init(lv_display_t * disp) {
    if (!disp) return;
    lv_theme_t * th = lv_theme_default_init(
        disp,
        accent_primary(),
        surface(),
        true, // dark mode
        &lv_font_roboto_14
    );
    lv_display_set_theme(disp, th);
}

void style_card(lv_obj_t * obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, surface_card(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, kCardRadius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, surface_border(), 0);
    lv_obj_set_style_pad_all(obj, 20, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void style_nav_rail(lv_obj_t * obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, kRailWidth, LV_PCT(100));
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x131519), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, surface_border(), 0);
    lv_obj_set_style_pad_ver(obj, 16, 0);
    lv_obj_set_style_pad_hor(obj, 8, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void style_nav_button(lv_obj_t * btn, bool active) {
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 52, 52);
    lv_obj_set_style_radius(btn, kPillRadius, 0);
    if (active) {
        lv_obj_set_style_bg_color(btn, accent_primary(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(btn, 16, 0);
        lv_obj_set_style_shadow_color(btn, accent_glow(), 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_color(btn, surface_container_high(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(btn, surface_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    style_focusable(btn);
}

void style_filter_chip(lv_obj_t * chip, bool active) {
    lv_obj_remove_style_all(chip);
    lv_obj_set_height(chip, 44);
    lv_obj_set_style_radius(chip, kPillRadius, 0);
    lv_obj_set_style_pad_hor(chip, 24, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (active) {
        lv_obj_set_style_bg_color(chip, accent_primary(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
    } else {
        lv_obj_set_style_bg_color(chip, surface_card(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_60, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, surface_border(), 0);
        lv_obj_set_style_bg_color(chip, surface_pressed(), LV_STATE_PRESSED);
    }
    style_focusable(chip);
}

void style_stepper_button(lv_obj_t * btn) {
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kStepperBtnSize, kStepperBtnSize);
    lv_obj_set_style_radius(btn, kPillRadius, 0);
    lv_obj_set_style_bg_color(btn, accent_primary(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6b9be8), LV_STATE_PRESSED);
    style_focusable(btn);
}

void style_level_bar(lv_obj_t * bar) {
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 200, 8);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, track_bg(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, track_fill(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

void style_focusable(lv_obj_t * obj) {
    lv_obj_set_style_outline_width(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, accent_primary(), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
}

} // namespace staging_ui::theme
