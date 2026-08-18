// Wireless Android Auto session integration -- talks to
// androidauto-sidecar (a separate process, see
// sidecars/androidauto/main.cpp) over a small local socket protocol
// via hal::AndroidAutoClient. This binary itself has NO aasdk/Boost/
// Protobuf knowledge -- see docs/ARCHITECTURE.md's carplay-sidecar
// section for the same "heavy dependency stays isolated in its own
// process" reasoning this mirrors.
#include "ui/android_auto_screen.h"

#include <utility>

#include "hal/androidauto_client.h"
#include "hal/display.h"
#include "hal/knob.h"
#include "core/navigation.h"
#include "ui/bluetooth_screen.h"
#include "ui/status_bar.h"
#include "ui/theme.h"
#include "ui/staging/nav_rail.h"
#include "ui/staging/theme.h"
#include "ui/staging/fonts.h"
#include "ui/staging/icons.h"

namespace ui {

namespace {

// Process-lifetime, intentionally never freed -- same convention as
// every other process-lifetime singleton in this codebase (e.g.
// bluetooth_screen.cpp's bt_handle()).
hal::AndroidAutoClient & client() {
    static hal::AndroidAutoClient c;
    return c;
}

struct ParsedStatus {
    std::string name;
    std::string detail;
};

// Parses androidauto_client.h's raw reply line ("STATE <name>
// <message...>", "OK", or "ERR <reason>") into a (name, detail) pair
// for display. Deliberately just string splitting -- the client
// intentionally has no shared enum with the sidecar (see
// androidauto_client.h's header comment on why), so this is the UI's
// own best-effort parse of the sidecar's text protocol, not a strict
// contract either side depends on.
ParsedStatus parse_status_line(const std::string & line) {
    if (line.rfind("STATE ", 0) == 0) {
        std::string rest = line.substr(6);
        auto sp = rest.find(' ');
        if (sp == std::string::npos) return {rest, ""};
        return {rest.substr(0, sp), rest.substr(sp + 1)};
    }
    if (line.rfind("ERR ", 0) == 0) {
        return {"Unreachable", line.substr(4)};
    }
    return {"Unknown", line};
}

lv_color_t color_for_state_name(const std::string & name) {
    if (name == "Connected") return staging_ui::theme::success();
    if (name == "Failed" || name == "Unreachable") return staging_ui::theme::danger();
    if (name == "Idle") return staging_ui::theme::text_secondary();
    return staging_ui::theme::accent_primary();
}

void connect_btn_cb(lv_event_t *) {
    client().requestConnect();
}

void bluetooth_btn_cb(lv_event_t *) {
    staging_ui::navigate_to(staging_ui::NavDestination::Bluetooth);
}

struct Widgets {
    // The whole instructions/status card -- see poll_timer_cb()'s
    // comment on why this gets hidden entirely (not just its
    // contents) once connected.
    lv_obj_t * content;
    lv_obj_t * state_label;
    lv_obj_t * detail_label;
    // Tracks whether hal::hide_display() has already been called, so
    // poll_timer_cb() only issues the ioctl on an actual Connected/
    // not-Connected transition rather than every 500ms tick.
    bool display_hidden = false;
};

// Polls the sidecar and refreshes the status widgets -- created
// against this screen, deleted alongside it (LV_EVENT_DELETE below) so
// it never fires against freed widgets after navigating away. The
// sidecar process itself (and whatever session it's driving) keeps
// running regardless of this screen's lifecycle.
void poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<Widgets *>(lv_timer_get_user_data(timer));

    ParsedStatus status = parse_status_line(client().statusLine());
    lv_label_set_text(w->state_label, status.name.c_str());
    lv_obj_set_style_text_color(w->state_label, color_for_state_name(status.name), 0);
    lv_label_set_text(w->detail_label, status.detail.c_str());

    hal::androidauto_screen_active().store(status.name == "Connected", std::memory_order_release);

    if (status.name == "Connected") {
        lv_obj_add_flag(w->content, LV_OBJ_FLAG_HIDDEN);
        if (!w->display_hidden) {
            hal::hide_display();
            w->display_hidden = true;
        }
    } else {
        lv_obj_clear_flag(w->content, LV_OBJ_FLAG_HIDDEN);
        if (w->display_hidden) {
            hal::show_display();
            w->display_hidden = false;
        }
    }
}

void screen_delete_cb(lv_event_t * e) {
    auto * pair = static_cast<std::pair<lv_timer_t *, Widgets *> *>(lv_event_get_user_data(e));
    lv_timer_delete(pair->first);
    if (pair->second->display_hidden) {
        hal::show_display();
    }
    delete pair->second;
    delete pair;
    client().setVisible(false);
    hal::androidauto_screen_active().store(false, std::memory_order_release);
}

}  // namespace

lv_obj_t * create_android_auto_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, staging_ui::theme::bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);

    // 1. Persistent 5-Icon Navigation Rail (Android Auto active)
    staging_ui::create_nav_rail(scr, staging_ui::NavDestination::AndroidAuto);

    client().setVisible(true);
    hal::androidauto_screen_active().store(false, std::memory_order_release);

    // 2. Main Content Card matching mockup_2_android_auto.jpg
    lv_obj_t * content = lv_obj_create(scr);
    staging_ui::theme::style_card(content);
    lv_obj_set_pos(content, staging_ui::theme::kRailWidth + 32, 20);
    lv_obj_set_size(content, 800 - (staging_ui::theme::kRailWidth + 64), 440);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, 24, 0);

    // Hero Icon
    lv_obj_t * hero_icon = ui::icons::create_icon(content, &ui::icons::icon_phone, staging_ui::theme::accent_primary());
    (void)hero_icon;

    // Title & Instructions
    lv_obj_t * title = lv_label_create(content);
    lv_label_set_text(title, "Ready to connect");
    lv_obj_set_style_text_font(title, &lv_font_roboto_24, 0);
    lv_obj_set_style_text_color(title, staging_ui::theme::text_primary(), 0);

    lv_obj_t * subtitle = lv_label_create(content);
    lv_label_set_text(subtitle, "Pair phone via Bluetooth to begin wireless session.");
    lv_obj_set_style_text_font(subtitle, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(subtitle, staging_ui::theme::text_secondary(), 0);

    // Connect (Wireless) CTA Button
    lv_obj_t * connect_btn = lv_button_create(content);
    lv_obj_remove_style_all(connect_btn);
    lv_obj_set_size(connect_btn, 420, 52);
    lv_obj_set_style_radius(connect_btn, staging_ui::theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(connect_btn, staging_ui::theme::accent_primary(), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x6b9be8), LV_STATE_PRESSED);
    staging_ui::theme::style_focusable(connect_btn);
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect (Wireless)");
    lv_obj_set_style_text_font(connect_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(connect_label, staging_ui::theme::text_on_accent(), 0);
    lv_obj_center(connect_label);

    // Open Bluetooth Secondary Button
    lv_obj_t * bluetooth_btn = lv_button_create(content);
    lv_obj_remove_style_all(bluetooth_btn);
    lv_obj_set_size(bluetooth_btn, 420, 52);
    lv_obj_set_style_radius(bluetooth_btn, staging_ui::theme::kPillRadius, 0);
    lv_obj_set_style_bg_color(bluetooth_btn, staging_ui::theme::surface_container_high(), 0);
    lv_obj_set_style_bg_opa(bluetooth_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bluetooth_btn, 1, 0);
    lv_obj_set_style_border_color(bluetooth_btn, staging_ui::theme::surface_border(), 0);
    staging_ui::theme::style_focusable(bluetooth_btn);
    lv_obj_add_event_cb(bluetooth_btn, bluetooth_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * bluetooth_btn_label = lv_label_create(bluetooth_btn);
    lv_label_set_text(bluetooth_btn_label, "Open Bluetooth");
    lv_obj_set_style_text_font(bluetooth_btn_label, &lv_font_roboto_20, 0);
    lv_obj_set_style_text_color(bluetooth_btn_label, staging_ui::theme::accent_primary(), 0);
    lv_obj_center(bluetooth_btn_label);

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), connect_btn);
        lv_group_add_obj(core::navigation::focus_group(), bluetooth_btn);
    }

    // Status Row
    lv_obj_t * status_row = lv_obj_create(content);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_size(status_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_row, 8, 0);

    ParsedStatus initial = parse_status_line(client().statusLine());
    lv_obj_t * state_body = lv_label_create(status_row);
    lv_label_set_text(state_body, initial.name.c_str());
    lv_obj_set_style_text_font(state_body, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(state_body, color_for_state_name(initial.name), 0);

    lv_obj_t * detail_body = lv_label_create(status_row);
    lv_label_set_text(detail_body, initial.detail.c_str());
    lv_obj_set_style_text_font(detail_body, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(detail_body, staging_ui::theme::text_secondary(), 0);

    if (initial.name == "Connected") {
        lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
    }

    auto * widgets = new Widgets{content, state_body, detail_body};
    lv_timer_t * timer = lv_timer_create(poll_timer_cb, 500, widgets);
    auto * pair = new std::pair<lv_timer_t *, Widgets *>(timer, widgets);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, pair);

    return scr;
}

}  // namespace ui
