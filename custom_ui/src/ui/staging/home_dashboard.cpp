#include "ui/staging/home_dashboard.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/settings_screen.h"
#include "ui/android_auto_screen.h"
#include "ui/carplay_screen.h"
#include "ui/bluetooth_screen.h"
#include "ui/reverse_camera_screen.h"
#include "ui/staging/icons.h"
#include "core/navigation.h"
#include "core/config_store.h"
#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "hal/mcu_input.h"
#include "core/log_timing.h"
#include <ctime>
#include <cstdio>
#include <string>

namespace staging_ui {

namespace {

struct DashboardWidgets {
    lv_obj_t * clock_lbl = nullptr;
    lv_obj_t * aa_title_lbl = nullptr;
    lv_obj_t * aa_status_lbl = nullptr;
    lv_obj_t * connect_lbl = nullptr;
    lv_obj_t * track_title_lbl = nullptr;
    lv_obj_t * track_artist_lbl = nullptr;
    lv_obj_t * playpause_icon = nullptr;
    lv_timer_t * poll_timer = nullptr;
};

hal::AndroidAutoClient & client() {
    static hal::AndroidAutoClient c;
    return c;
}

void update_clock(DashboardWidgets * w) {
    if (!w || !w->clock_lbl) return;
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%I:%M %p", &local);
    const char * formatted = (buf[0] == '0') ? &buf[1] : buf;
    lv_label_set_text(w->clock_lbl, formatted);
}

void update_dashboard_status(DashboardWidgets * w) {
    if (!w) return;
    update_clock(w);

    std::string proj = core::default_store().get_string("ProjectionType", "AndroidAuto", "General");
    bool is_carplay = (proj == "CarPlay");

    if (w->aa_title_lbl) {
        lv_label_set_text(w->aa_title_lbl, is_carplay ? "Apple CarPlay" : "Android Auto");
    }

    if (w->aa_status_lbl && w->connect_lbl) {
        if (is_carplay) {
            FILE * f = fopen("/tmp/carplay", "r");
            bool is_linked = (f != nullptr);
            if (f) fclose(f);

            if (is_linked) {
                lv_label_set_text(w->aa_status_lbl, "Session: Active (Connected)");
                lv_label_set_text(w->connect_lbl, "Open Session");
            } else {
                lv_label_set_text(w->aa_status_lbl, "Connection: Ready to pair");
                lv_label_set_text(w->connect_lbl, "Quick Connect");
            }
        } else {
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
    }

    if (!w->track_title_lbl || !w->track_artist_lbl) return;
    hal::BluetoothTelemetry telem = hal::get_telemetry();
    if (!telem.connected || telem.track_title.empty()) {
        lv_label_set_text(w->track_title_lbl, "Not Playing");
        lv_label_set_text(w->track_artist_lbl, telem.connected ? telem.connected_device_name.c_str() : "No device connected");
    } else {
        lv_label_set_text(w->track_title_lbl, telem.track_title.c_str());
        lv_label_set_text(w->track_artist_lbl, telem.track_artist.empty() ? " " : telem.track_artist.c_str());
    }
    if (w->playpause_icon) {
        lv_image_set_src(w->playpause_icon, telem.play_status == 1 ? &ui::icons::icon_pause : &ui::icons::icon_play);
    }
}

void poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<DashboardWidgets *>(lv_timer_get_user_data(timer));
    update_dashboard_status(w);
}

void quick_connect_clicked_cb(lv_event_t * e) {
    (void)e;
    std::string proj = core::default_store().get_string("ProjectionType", "AndroidAuto", "General");
    if (proj == "CarPlay") {
        core::navigation::push(ui::create_carplay_screen);
    } else {
        std::string line = client().statusLine(false);
        if (line.rfind("STATE Connected", 0) != 0) {
            client().requestConnect();
        }
        core::navigation::push(ui::create_android_auto_screen);
    }
}

void media_prev_clicked_cb(lv_event_t * /*e*/) {
    hal::media_prev_track(hal::shared_handle());
}

void media_play_pause_clicked_cb(lv_event_t * e) {
    hal::media_play_pause(hal::shared_handle());
    auto * w = static_cast<DashboardWidgets *>(lv_event_get_user_data(e));
    update_dashboard_status(w);
}

void media_next_clicked_cb(lv_event_t * /*e*/) {
    hal::media_next_track(hal::shared_handle());
}

// 2026-09-04: real hardware need -- returning to the OEM Factory LCD
// previously required a physical HOME-button long-press (~1.4s hold,
// see docs/MCU_COMMAND_REFERENCE.md's CMD 0x12 investigation). Wires
// the same real, hardware-confirmed relay command custom_ui already
// sends for the "Aftermarket Reverse Camera" setting toggle
// (hal::send_mcu_video_relay(), CMD 0xA0 id=0x00/id=0x11 + CMD 0x84 --
// see that function's own header comment for the full real-hardware
// trace) to a direct on-screen button instead. hal::send_mcu_video_relay()
// itself is a few fast, non-blocking write()s to the MCU serial port
// (not a socket/IPC round-trip like AndroidAutoClient's calls), so
// calling it directly here on the LVGL thread is safe, same as every
// other settings-screen toggle in this codebase already does.
//
// Real, honest caveat, not yet resolved by a hardware test: this
// command is confirmed (real, methodical testing) to control which
// camera relay engages *when reverse gear transitions* -- it has NOT
// yet been confirmed whether sending it alone, with no gear
// transition happening, forces an immediate switch of whatever's
// currently on screen, or only arms a preference for the next real
// transition (matching CMD 0xA0 id=0x11's own confirmed arm-then-
// trigger gating). This button is the direct way to find out.
void factory_lcd_clicked_cb(lv_event_t * /*e*/) {
    std::printf("%s [UI] Home dashboard: \"Return to Factory LCD\" pressed -- sending hal::send_mcu_video_relay(true)\n",
                core::log_timestamp().c_str());
    hal::send_mcu_video_relay(true);
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
    lv_obj_set_style_text_font(clock_lbl, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(clock_lbl, theme::text_primary(), 0);
    widgets->clock_lbl = clock_lbl;
    update_clock(widgets);

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
    widgets->aa_title_lbl = aa_title;

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
    lv_label_set_text(audio_title, "Now Playing");
    lv_obj_set_style_text_font(audio_title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(audio_title, theme::text_primary(), 0);

    // Track Title / Artist (or Bluetooth device name / connection state
    // when nothing is playing) -- see hal::get_telemetry()'s own
    // BluetoothTelemetry struct for where these come from (AVRCP /
    // BlueZ MediaControl1 track metadata, same source status_bar.cpp
    // already reads for the bar's connection glyph).
    lv_obj_t * track_box = lv_obj_create(card_audio);
    lv_obj_remove_style_all(track_box);
    lv_obj_set_width(track_box, LV_PCT(100));
    lv_obj_set_flex_grow(track_box, 1);
    lv_obj_set_flex_flow(track_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(track_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(track_box, 8, 0);
    lv_obj_clear_flag(track_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * track_title_lbl = lv_label_create(track_box);
    lv_label_set_text(track_title_lbl, "Not Playing");
    lv_label_set_long_mode(track_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(track_title_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(track_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(track_title_lbl, &lv_font_roboto_24, 0);
    lv_obj_set_style_text_color(track_title_lbl, theme::text_primary(), 0);
    widgets->track_title_lbl = track_title_lbl;

    lv_obj_t * track_artist_lbl = lv_label_create(track_box);
    lv_label_set_text(track_artist_lbl, "No device connected");
    lv_label_set_long_mode(track_artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(track_artist_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(track_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(track_artist_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(track_artist_lbl, theme::text_secondary(), 0);
    widgets->track_artist_lbl = track_artist_lbl;

    // Bottom Playback Controls Row (Prev / Play / Next)
    lv_obj_t * ctrl_row = lv_obj_create(card_audio);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_width(ctrl_row, LV_PCT(100));
    lv_obj_set_height(ctrl_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 16, 0);

    auto make_ctrl_btn = [ctrl_row, widgets](const lv_image_dsc_t * dsc, lv_event_cb_t click_cb) -> lv_obj_t * {
        lv_obj_t * btn = lv_button_create(ctrl_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 48, 48);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, theme::surface_container_high(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, theme::surface_pressed(), LV_STATE_PRESSED);
        theme::style_focusable(btn);
        lv_obj_add_event_cb(btn, click_cb, LV_EVENT_CLICKED, widgets);

        lv_obj_t * icon = ui::icons::create_icon(btn, dsc, theme::text_primary());
        lv_obj_center(icon);

        if (core::navigation::focus_group()) {
            lv_group_add_obj(core::navigation::focus_group(), btn);
        }
        return icon;
    };

    make_ctrl_btn(&ui::icons::icon_prev, media_prev_clicked_cb);
    widgets->playpause_icon = make_ctrl_btn(&ui::icons::icon_play, media_play_pause_clicked_cb);
    make_ctrl_btn(&ui::icons::icon_next, media_next_clicked_cb);

    // "Return to Factory LCD" -- see factory_lcd_clicked_cb's own
    // comment for the real hardware context. A slim footer row below
    // the two dashboard cards; main_area is flex-column with cards_row
    // set to flex_grow(1), so this fixed-height sibling just takes its
    // own space at the bottom without needing to touch that layout.
    lv_obj_t * factory_lcd_btn = lv_button_create(main_area);
    lv_obj_remove_style_all(factory_lcd_btn);
    lv_obj_set_size(factory_lcd_btn, LV_PCT(100), 44);
    lv_obj_set_style_radius(factory_lcd_btn, theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(factory_lcd_btn, theme::surface_container_high(), 0);
    lv_obj_set_style_bg_opa(factory_lcd_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(factory_lcd_btn, theme::surface_pressed(), LV_STATE_PRESSED);
    theme::style_focusable(factory_lcd_btn);
    lv_obj_add_event_cb(factory_lcd_btn, factory_lcd_clicked_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * factory_lcd_lbl = lv_label_create(factory_lcd_btn);
    lv_label_set_text(factory_lcd_lbl, "Return to Factory LCD");
    lv_obj_set_style_text_font(factory_lcd_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(factory_lcd_lbl, theme::text_primary(), 0);
    lv_obj_center(factory_lcd_lbl);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), factory_lcd_btn);
    }

    // Initial Status Check & Periodic Poll
    update_dashboard_status(widgets);
    widgets->poll_timer = lv_timer_create(poll_timer_cb, 1000, widgets);

    return scr;
}

} // namespace staging_ui
