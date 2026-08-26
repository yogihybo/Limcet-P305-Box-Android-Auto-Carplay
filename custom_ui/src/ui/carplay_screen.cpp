#include "ui/carplay_screen.h"

#include <atomic>
#include <utility>
#include <cstdio>

#include "hal/display.h"
#include "hal/knob.h"
#include "core/log_timing.h"
#include "core/navigation.h"
#include "core/config_store.h"
#include "ui/bluetooth_screen.h"
#include "ui/theme.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/icons.h"

namespace ui {

namespace {

struct CarPlayWidgets {
    lv_obj_t * content = nullptr;
    lv_obj_t * state_lbl = nullptr;
    lv_obj_t * title_lbl = nullptr;
    lv_obj_t * subtitle_lbl = nullptr;
    lv_obj_t * action_btn = nullptr;
    lv_obj_t * action_label = nullptr;
    bool connected = false;
};

void carplay_poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<CarPlayWidgets *>(lv_timer_get_user_data(timer));
    if (!w) return;

    // Check /tmp/carplay file indicator (created by /usr/bin/carplay when active)
    FILE * f = fopen("/tmp/carplay", "r");
    bool is_linked = (f != nullptr);
    if (f) fclose(f);

    if (is_linked && !w->connected) {
        w->connected = true;
        lv_label_set_text(w->title_lbl, "Apple CarPlay Active");
        lv_label_set_text(w->subtitle_lbl, "Projection session running.");
        lv_label_set_text(w->state_lbl, "Connected");
        lv_obj_set_style_text_color(w->state_lbl, staging_ui::theme::success(), 0);
        lv_label_set_text(w->action_label, "Resume CarPlay");
    } else if (!is_linked && w->connected) {
        w->connected = false;
        lv_label_set_text(w->title_lbl, "Apple CarPlay");
        lv_label_set_text(w->subtitle_lbl, "Connect iPhone via USB or 5GHz Wi-Fi to start CarPlay.");
        lv_label_set_text(w->state_lbl, "Ready to connect");
        lv_obj_set_style_text_color(w->state_lbl, staging_ui::theme::accent_primary(), 0);
        lv_label_set_text(w->action_label, "Pair iPhone (Bluetooth)");
    }
}

void action_btn_cb(lv_event_t *) {
    core::navigation::push(ui::create_bluetooth_screen);
}

void carplay_screen_delete_cb(lv_event_t * e) {
    auto * pair = static_cast<std::pair<lv_timer_t *, CarPlayWidgets *> *>(lv_event_get_user_data(e));
    if (pair) {
        if (pair->first) lv_timer_delete(pair->first);
        delete pair->second;
        delete pair;
    }
}

}  // namespace

lv_obj_t * create_carplay_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, staging_ui::theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);

    // 1. Persistent 5-Icon Navigation Rail (Navigation active)
    staging_ui::create_nav_rail(scr, staging_ui::NavDestination::AndroidAuto);

    // 2. Main Content Card
    lv_obj_t * content = lv_obj_create(scr);
    staging_ui::theme::style_card(content);
    lv_obj_set_pos(content, staging_ui::theme::kRailWidth + 16, 8);
    lv_obj_set_size(content, 800 - (staging_ui::theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, 32, 0);

    // Title
    lv_obj_t * title = lv_label_create(content);
    lv_label_set_text(title, "Apple CarPlay");
    lv_obj_set_style_text_font(title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(title, staging_ui::theme::text_primary(), 0);

    // Subtitle / Instructions
    lv_obj_t * subtitle = lv_label_create(content);
    lv_label_set_text(subtitle, "Connect iPhone via USB or 5GHz Wi-Fi to start CarPlay.");
    lv_obj_set_style_text_font(subtitle, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(subtitle, staging_ui::theme::text_secondary(), 0);

    // Action Button
    lv_obj_t * action_btn = lv_button_create(content);
    lv_obj_remove_style_all(action_btn);
    lv_obj_set_size(action_btn, 360, 56);
    lv_obj_set_style_radius(action_btn, staging_ui::theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(action_btn, staging_ui::theme::accent_primary(), 0);
    lv_obj_set_style_bg_opa(action_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(action_btn, lv_color_hex(0x6b9be8), LV_STATE_PRESSED);
    staging_ui::theme::style_focusable(action_btn);
    lv_obj_add_event_cb(action_btn, action_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * action_label = lv_label_create(action_btn);
    lv_label_set_text(action_label, "Pair iPhone (Bluetooth)");
    lv_obj_set_style_text_font(action_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(action_label, staging_ui::theme::text_on_accent(), 0);
    lv_obj_center(action_label);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), action_btn);
    }

    // Status Row
    lv_obj_t * status_row = lv_obj_create(content);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 8, 0);

    lv_obj_t * state_lbl = lv_label_create(status_row);
    lv_label_set_text(state_lbl, "Ready to connect");
    lv_obj_set_style_text_font(state_lbl, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(state_lbl, staging_ui::theme::accent_primary(), 0);

    auto * widgets = new CarPlayWidgets{content, state_lbl, title, subtitle, action_btn, action_label, false};
    lv_timer_t * timer = lv_timer_create(carplay_poll_timer_cb, 500, widgets);
    auto * pair = new std::pair<lv_timer_t *, CarPlayWidgets *>(timer, widgets);
    lv_obj_add_event_cb(scr, carplay_screen_delete_cb, LV_EVENT_DELETE, pair);

    return scr;
}

}  // namespace ui
