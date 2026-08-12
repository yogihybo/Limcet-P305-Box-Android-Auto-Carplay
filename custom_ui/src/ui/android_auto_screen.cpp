// Wireless Android Auto session integration -- talks to
// androidauto-sidecar (a separate process, see
// sidecars/androidauto/main.cpp) over a small local socket protocol
// via hal::AndroidAutoClient. This binary itself has NO aasdk/Boost/
// Protobuf knowledge -- see docs/ARCHITECTURE.md's carplay-sidecar
// section for the same "heavy dependency stays isolated in its own
// process" reasoning this mirrors.
#include "ui/android_auto_screen.h"

#include "hal/androidauto_client.h"
#include "core/navigation.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

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
    if (name == "Connected") return theme::success();
    if (name == "Failed" || name == "Unreachable") return theme::danger();
    if (name == "Idle") return theme::text_secondary();
    return theme::accent();
}

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

void connect_btn_cb(lv_event_t *) {
    client().requestConnect();
}

// Polls the sidecar and refreshes the status widgets -- created
// against this screen, deleted alongside it (LV_EVENT_DELETE below) so
// it never fires against freed widgets after navigating away. The
// sidecar process itself (and whatever session it's driving) keeps
// running regardless of this screen's lifecycle.
void poll_timer_cb(lv_timer_t * timer) {
    auto * widgets = static_cast<lv_obj_t **>(lv_timer_get_user_data(timer));
    lv_obj_t * state_label_obj = widgets[0];
    lv_obj_t * detail_label_obj = widgets[1];

    ParsedStatus status = parse_status_line(client().statusLine());
    lv_label_set_text(state_label_obj, status.name.c_str());
    lv_obj_set_style_text_color(state_label_obj, color_for_state_name(status.name), 0);
    lv_label_set_text(detail_label_obj, status.detail.c_str());
}

void screen_delete_cb(lv_event_t * e) {
    auto * timer = static_cast<lv_timer_t *>(lv_event_get_user_data(e));
    lv_timer_delete(timer);
    // 2026-08-12: hide the AA video hardware layer the moment this
    // screen goes away -- see hal::AndroidAutoClient::setVisible()'s
    // own comment. Session/decode keep running in the background
    // regardless (auto-start, see main.cpp's AaAutoStartWatcher); only
    // the hardware layer's visibility is tied to this screen.
    client().setVisible(false);
}

}  // namespace

lv_obj_t * create_android_auto_screen() {
    lv_obj_t * scr = nullptr;
    theme::create_screen_with_header(&scr, "Android Auto", back_btn_cb);

    // 2026-08-12: reveals the AA video hardware layer (if a session is
    // already running -- e.g. auto-started in the background, see
    // main.cpp's AaAutoStartWatcher) the moment this screen is
    // selected, per explicit request: selecting the AA icon should
    // load the video feed directly, not just show a status screen
    // while video stays hidden. No-op (returns false, logged nowhere
    // since it's not an error) if no sidecar/session exists yet --
    // this doesn't start one, connect_btn_cb()/AutoStartCarLink do
    // that; this only ever affects visibility.
    client().setVisible(true);

    lv_obj_t * content = lv_obj_create(scr);
    theme::style_card(content);
    // Bottom offset -10 -> -(status_bar::kHeight + 8): the status bar
    // (ui/status_bar.h) now sits at the literal bottom of the screen,
    // so this card needs to stop short of it instead of running to the
    // screen edge. Height 72% -> 66% to match.
    lv_obj_set_size(content, LV_PCT(90), LV_PCT(66));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -(status_bar::kHeight + 8));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 16, 0);
    lv_obj_set_style_pad_row(content, 10, 0);

    lv_obj_t * connect_btn = lv_button_create(content);
    theme::style_primary_button(connect_btn);
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), connect_btn);
    lv_obj_t * connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect (Wireless)");

    lv_obj_t * state_header = lv_label_create(content);
    lv_label_set_text(state_header, "Status");
    theme::style_section_label(state_header);

    ParsedStatus initial = parse_status_line(client().statusLine());
    lv_obj_t * state_body = lv_label_create(content);
    lv_label_set_text(state_body, initial.name.c_str());
    lv_obj_set_style_text_color(state_body, color_for_state_name(initial.name), 0);

    lv_obj_t * detail_body = lv_label_create(content);
    lv_label_set_long_mode(detail_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_body, LV_PCT(100));
    lv_label_set_text(detail_body, initial.detail.c_str());
    lv_obj_set_style_text_color(detail_body, lv_color_hex(0xcccccc), 0);

    lv_obj_t * how_header = lv_label_create(content);
    lv_label_set_text(how_header, "Notes");
    theme::style_section_label(how_header);

    lv_obj_t * how_body = lv_label_create(content);
    lv_label_set_long_mode(how_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(how_body, LV_PCT(100));
    lv_label_set_text(how_body,
                       "Wireless only for now (this device's one external USB port is "
                       "normally occupied by the boot rootfs drive). Driven by the "
                       "androidauto-sidecar process -- if status shows \"Unreachable\", "
                       "that process isn't running. Starts this app's own WiFi AP (SSID "
                       "\"carplay_wifi\"), then a Bluetooth-relayed credential handoff -- "
                       "the phone must already be Bluetooth-paired first from the "
                       "Bluetooth screen. Not yet hardware-tested end to end -- video/audio "
                       "channels aren't implemented yet even once connected.");
    theme::style_secondary_text(how_body);

    // widgets[] intentionally leaked for the screen's lifetime, same
    // pattern as every other process-lifetime singleton array in this
    // codebase (e.g. bluetooth_screen.cpp's device-list refresh
    // widgets) -- ScreenManager::pop() deletes the LVGL objects
    // themselves but this plain heap block isn't LVGL-owned.
    auto * widgets = new lv_obj_t *[2]{state_body, detail_body};
    lv_timer_t * timer = lv_timer_create(poll_timer_cb, 500, widgets);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, timer);

    return scr;
}

}  // namespace ui
