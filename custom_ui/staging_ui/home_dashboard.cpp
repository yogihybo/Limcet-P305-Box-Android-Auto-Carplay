#include "staging_ui/home_dashboard.h"
#include "staging_ui/theme.h"
#include "staging_ui/fonts.h"
#include "staging_ui/nav_rail.h"
#include "staging_ui/settings_screen.h"
#include "core/navigation.h"
#include "ui/android_auto_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"
#include <ctime>
#include <cstdio>

namespace staging_ui {

namespace {

void quick_connect_clicked_cb(lv_event_t *) {
    core::navigation::push(ui::create_android_auto_screen);
}

} // namespace

lv_obj_t * create_home_dashboard() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 1. Persistent 5-Icon Navigation Rail (Home is active)
    create_nav_rail(scr, NavDestination::Home, [](NavDestination dest) {
        switch (dest) {
            case NavDestination::AndroidAuto:
                core::navigation::push(ui::create_android_auto_screen);
                break;
            case NavDestination::Bluetooth:
                core::navigation::push(ui::create_bluetooth_screen);
                break;
            case NavDestination::Camera:
                core::navigation::push(ui::create_reverse_camera_screen);
                break;
            case NavDestination::Settings:
                core::navigation::push(create_settings_screen);
                break;
            case NavDestination::Home:
                break;
        }
    });

    // 2. Main Dashboard Area
    lv_obj_t * main_area = lv_obj_create(scr);
    lv_obj_remove_style_all(main_area);
    lv_obj_set_pos(main_area, theme::kRailWidth + 16, 8);
    lv_obj_set_size(main_area, 800 - (theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(main_area, 12, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    // Top Status Header (Time + Status)
    lv_obj_t * header = lv_obj_create(main_area);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 28);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * clock_lbl = lv_label_create(header);
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(clock_lbl, buf);
    lv_obj_set_style_text_font(clock_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(clock_lbl, theme::text_primary(), 0);

    // Dual-Card Container
    lv_obj_t * cards_row = lv_obj_create(main_area);
    lv_obj_remove_style_all(cards_row);
    lv_obj_set_width(cards_row, LV_PCT(100));
    lv_obj_set_flex_grow(cards_row, 1);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cards_row, 16, 0);
    lv_obj_clear_flag(cards_row, LV_OBJ_FLAG_SCROLLABLE);

    // Card 1: Android Auto Hero Card
    lv_obj_t * card_aa = lv_obj_create(cards_row);
    theme::style_card(card_aa);
    lv_obj_set_width(card_aa, 332);
    lv_obj_set_height(card_aa, LV_PCT(100));
    lv_obj_set_flex_flow(card_aa, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_aa, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card_aa, 24, 0);

    lv_obj_t * aa_title = lv_label_create(card_aa);
    lv_label_set_text(aa_title, "Android Auto");
    lv_obj_set_style_text_font(aa_title, &lv_font_roboto_24, 0);
    lv_obj_set_style_text_color(aa_title, theme::text_primary(), 0);

    lv_obj_t * aa_status = lv_label_create(card_aa);
    lv_label_set_text(aa_status, "Connection: Ready to pair");
    lv_obj_set_style_text_font(aa_status, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(aa_status, theme::text_secondary(), 0);

    lv_obj_t * connect_btn = lv_button_create(card_aa);
    lv_obj_remove_style_all(connect_btn);
    lv_obj_set_size(connect_btn, LV_PCT(100), 56);
    lv_obj_set_style_radius(connect_btn, theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(connect_btn, theme::accent_primary(), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x6b9be8), LV_STATE_PRESSED);
    theme::style_focusable(connect_btn);
    lv_obj_add_event_cb(connect_btn, quick_connect_clicked_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "Quick Connect");
    lv_obj_set_style_text_font(connect_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(connect_lbl, theme::text_on_accent(), 0);
    lv_obj_center(connect_lbl);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), connect_btn);
    }

    // Card 2: Audio Volume Card
    lv_obj_t * card_audio = lv_obj_create(cards_row);
    theme::style_card(card_audio);
    lv_obj_set_width(card_audio, 332);
    lv_obj_set_height(card_audio, LV_PCT(100));
    lv_obj_set_flex_flow(card_audio, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_audio, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card_audio, 24, 0);

    lv_obj_t * audio_title = lv_label_create(card_audio);
    lv_label_set_text(audio_title, "Audio Volume");
    lv_obj_set_style_text_font(audio_title, &lv_font_roboto_24, 0);
    lv_obj_set_style_text_color(audio_title, theme::text_primary(), 0);

    // Arc Volume Gauge
    lv_obj_t * arc = lv_arc_create(card_audio);
    lv_obj_set_size(arc, 160, 160);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 65);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, theme::track_bg(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::accent_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_center(arc);

    // Playback Controls Row (Prev / Play / Next)
    lv_obj_t * ctrl_row = lv_obj_create(card_audio);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_width(ctrl_row, LV_PCT(100));
    lv_obj_set_height(ctrl_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto add_ctrl_btn = [ctrl_row](const char * sym) {
        lv_obj_t * b = lv_button_create(ctrl_row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 44, 44);
        lv_obj_set_style_radius(b, theme::kPillRadius, 0);
        lv_obj_set_style_bg_color(b, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        theme::style_focusable(b);

        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text(l, sym);
        lv_obj_set_style_text_font(l, &lv_font_roboto_20, 0);
        lv_obj_set_style_text_color(l, theme::text_primary(), 0);
        lv_obj_center(l);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), b);
        }
    };

    add_ctrl_btn(LV_SYMBOL_PREV);
    add_ctrl_btn(LV_SYMBOL_PAUSE);
    add_ctrl_btn(LV_SYMBOL_NEXT);

    return scr;
}

} // namespace staging_ui
