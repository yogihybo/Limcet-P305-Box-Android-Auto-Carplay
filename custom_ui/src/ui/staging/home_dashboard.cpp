#include "ui/staging/home_dashboard.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/settings_screen.h"
#include "ui/android_auto_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"
#include "ui/staging/icons.h"
#include "core/navigation.h"
#include "hal/androidauto_client.h"
#include <ctime>
#include <cstdio>
#include <string>

namespace staging_ui {

namespace {

struct DashboardWidgets {
    lv_obj_t * aa_status_lbl = nullptr;
    lv_obj_t * connect_lbl = nullptr;
    lv_timer_t * poll_timer = nullptr;
};

hal::AndroidAutoClient & client() {
    static hal::AndroidAutoClient c;
    return c;
}

void update_dashboard_status(DashboardWidgets * w) {
    if (!w || !w->aa_status_lbl || !w->connect_lbl) return;
    std::string line = client().statusLine(false);
    if (line.rfind("STATE Connected", 0) == 0) {
        lv_label_set_text(w->aa_status_lbl, "Session: Active (Connected)");
        lv_label_set_text(w->connect_lbl, "Open Session");
    } else if (line.rfind("STATE Connecting", 0) == 0) {
        lv_label_set_text(w->aa_status_lbl, "Connection: Connecting...");
        lv_label_set_text(w->connect_lbl, "Connecting...");
    } else {
        lv_label_set_text(w->aa_status_lbl, "Connection: Ready to pair");
        lv_label_set_text(w->connect_lbl, "Quick Connect");
    }
}

void poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<DashboardWidgets *>(lv_timer_get_user_data(timer));
    update_dashboard_status(w);
}

void quick_connect_clicked_cb(lv_event_t * e) {
    std::string line = client().statusLine(false);
    if (line.rfind("STATE Connected", 0) == 0) {
        core::navigation::push(ui::create_android_auto_screen);
    } else {
        client().requestConnect();
        core::navigation::push(ui::create_android_auto_screen);
    }
}

void dashboard_delete_cb(lv_event_t * e) {
    auto * w = static_cast<DashboardWidgets *>(lv_event_get_user_data(e));
    if (w) {
        if (w->poll_timer) {
            lv_timer_delete(w->poll_timer);
        }
        delete w;
    }
}

} // namespace

lv_obj_t * create_home_dashboard() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    auto * widgets = new DashboardWidgets();
    lv_obj_add_event_cb(scr, dashboard_delete_cb, LV_EVENT_DELETE, widgets);

    // 1. Persistent 5-Icon Navigation Rail (Home is active)
    create_nav_rail(scr, NavDestination::Home);

    // 2. Main Dashboard Area
    lv_obj_t * main_area = lv_obj_create(scr);
    lv_obj_remove_style_all(main_area);
    lv_obj_set_pos(main_area, theme::kRailWidth + 16, 8);
    lv_obj_set_size(main_area, 800 - (theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(main_area, 8, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    // Top Status Header (Time Centered)
    lv_obj_t * header = lv_obj_create(main_area);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 34);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * clock_lbl = lv_label_create(header);
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(clock_lbl, buf);
    lv_obj_set_style_text_font(clock_lbl, &lv_font_roboto_28, 0);
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

    // Card 1: Android Auto Hero Card (336px wide x 416px high)
    lv_obj_t * card_aa = lv_obj_create(cards_row);
    theme::style_card(card_aa);
    lv_obj_set_width(card_aa, 336);
    lv_obj_set_height(card_aa, LV_PCT(100));
    lv_obj_set_flex_flow(card_aa, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_aa, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card_aa, 24, 0);

    // Header container with Title & Status (No icon in front of title to match mockup)
    lv_obj_t * aa_header_box = lv_obj_create(card_aa);
    lv_obj_remove_style_all(aa_header_box);
    lv_obj_set_size(aa_header_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(aa_header_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(aa_header_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(aa_header_box, 14, 0);

    lv_obj_t * aa_title = lv_label_create(aa_header_box);
    lv_label_set_text(aa_title, "Android Auto");
    lv_obj_set_style_text_font(aa_title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(aa_title, theme::text_primary(), 0);

    lv_obj_t * aa_status = lv_label_create(aa_header_box);
    lv_label_set_text(aa_status, "Connection: Ready to pair");
    lv_obj_set_style_text_font(aa_status, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(aa_status, theme::text_secondary(), 0);
    widgets->aa_status_lbl = aa_status;

    // Bottom "Quick Connect" stadium pill button
    lv_obj_t * connect_btn = lv_button_create(card_aa);
    lv_obj_remove_style_all(connect_btn);
    lv_obj_set_size(connect_btn, LV_PCT(100), 56);
    lv_obj_set_style_radius(connect_btn, theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(connect_btn, theme::accent_primary(), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x6b9be8), LV_STATE_PRESSED);
    theme::style_focusable(connect_btn);
    lv_obj_add_event_cb(connect_btn, quick_connect_clicked_cb, LV_EVENT_CLICKED, widgets);

    lv_obj_t * connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "Quick Connect");
    lv_obj_set_style_text_font(connect_lbl, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(connect_lbl, theme::text_on_accent(), 0);
    lv_obj_center(connect_lbl);
    widgets->connect_lbl = connect_lbl;

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), connect_btn);
    }

    // Card 2: Audio Volume Card (336px wide x 416px high)
    lv_obj_t * card_audio = lv_obj_create(cards_row);
    theme::style_card(card_audio);
    lv_obj_set_width(card_audio, 336);
    lv_obj_set_height(card_audio, LV_PCT(100));
    lv_obj_set_flex_flow(card_audio, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_audio, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card_audio, 24, 0);

    // Title (No icon in front of title to match mockup)
    lv_obj_t * audio_title = lv_label_create(card_audio);
    lv_label_set_text(audio_title, "Audio Volume");
    lv_obj_set_style_text_font(audio_title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(audio_title, theme::text_primary(), 0);

    // Center Arc Volume Gauge
    lv_obj_t * arc = lv_arc_create(card_audio);
    lv_obj_set_size(arc, 180, 180);
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

    // Bottom Playback Controls Row (Prev / Play / Next)
    lv_obj_t * ctrl_row = lv_obj_create(card_audio);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_width(ctrl_row, LV_PCT(100));
    lv_obj_set_height(ctrl_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 16, 0);

    auto make_ctrl_btn = [ctrl_row](const lv_image_dsc_t * dsc) -> lv_obj_t * {
        lv_obj_t * btn = lv_button_create(ctrl_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 48, 48);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, theme::surface_pressed(), LV_STATE_PRESSED);
        theme::style_focusable(btn);

        lv_obj_t * icon = ui::icons::create_icon(btn, dsc, theme::text_primary());
        lv_obj_center(icon);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
        return btn;
    };

    make_ctrl_btn(&ui::icons::icon_prev);
    make_ctrl_btn(&ui::icons::icon_pause);
    make_ctrl_btn(&ui::icons::icon_next);

    // Initial Status Check & Periodic Poll
    update_dashboard_status(widgets);
    widgets->poll_timer = lv_timer_create(poll_timer_cb, 1000, widgets);

    return scr;
}

} // namespace staging_ui
