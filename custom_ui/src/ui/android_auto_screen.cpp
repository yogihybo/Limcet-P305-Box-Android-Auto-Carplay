// Wireless Android Auto session integration -- see
// src/androidauto/wireless_session_manager.h for the full real
// protocol/architecture this screen drives: AP bring-up
// (firmware_overlay/etc/wifi_ap.sh) -> Bluetooth-relayed credential
// handoff (BwAapClient) -> phone joins the AP -> aasdk Session over
// TCPTransport. NOT YET hardware-tested end to end.
#include "ui/android_auto_screen.h"

#include "androidauto/wireless_session_manager.h"
#include "core/navigation.h"

namespace ui {

namespace {

// Process-lifetime, intentionally never freed -- same convention as
// every other process-lifetime singleton in this codebase (e.g.
// bluetooth_screen.cpp's bt_handle()). Deliberately NOT tied to this
// screen's own lifetime: once a session starts connecting, navigating
// away and back shouldn't tear it down or restart it.
androidauto::WirelessSessionManager & session_manager() {
    static androidauto::WirelessSessionManager manager;
    return manager;
}

const char * state_label(androidauto::WirelessSessionState s) {
    switch (s) {
        case androidauto::WirelessSessionState::Idle: return "Idle";
        case androidauto::WirelessSessionState::StartingAccessPoint: return "Starting WiFi AP";
        case androidauto::WirelessSessionState::BluetoothHandshake: return "Bluetooth handshake";
        case androidauto::WirelessSessionState::WaitingForWifiJoin: return "Waiting for phone";
        case androidauto::WirelessSessionState::Connecting: return "Connecting";
        case androidauto::WirelessSessionState::Connected: return "Connected";
        case androidauto::WirelessSessionState::Failed: return "Failed";
    }
    return "Unknown";
}

lv_color_t state_color(androidauto::WirelessSessionState s) {
    switch (s) {
        case androidauto::WirelessSessionState::Connected: return lv_color_hex(0x4caf50);
        case androidauto::WirelessSessionState::Failed: return lv_color_hex(0xe05252);
        case androidauto::WirelessSessionState::Idle: return lv_color_hex(0x999999);
        default: return lv_color_hex(0x66aaff);
    }
}

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
}

void connect_btn_cb(lv_event_t *) {
    session_manager().start();
}

// Polls session_manager() and refreshes the status widgets -- created
// against this screen, deleted alongside it (LV_EVENT_DELETE below) so
// it never fires against freed widgets after navigating away.
// session_manager() itself keeps running regardless (see its own
// comment).
void poll_timer_cb(lv_timer_t * timer) {
    auto * widgets = static_cast<lv_obj_t **>(lv_timer_get_user_data(timer));
    lv_obj_t * state_label_obj = widgets[0];
    lv_obj_t * detail_label_obj = widgets[1];

    auto state = session_manager().state();
    lv_label_set_text(state_label_obj, state_label(state));
    lv_obj_set_style_text_color(state_label_obj, state_color(state), 0);
    lv_label_set_text(detail_label_obj, session_manager().statusMessage().c_str());
}

void screen_delete_cb(lv_event_t * e) {
    auto * timer = static_cast<lv_timer_t *>(lv_event_get_user_data(e));
    lv_timer_delete(timer);
}

}  // namespace

lv_obj_t * create_android_auto_screen() {
    lv_obj_t * scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x14141e), 0);

    lv_obj_t * back_btn = lv_button_create(scr);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Android Auto");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(90), LV_PCT(75));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);

    lv_obj_t * connect_btn = lv_button_create(content);
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect (Wireless)");

    lv_obj_t * state_header = lv_label_create(content);
    lv_label_set_text(state_header, "Status");
    lv_obj_set_style_text_color(state_header, lv_color_hex(0x66aaff), 0);

    lv_obj_t * state_body = lv_label_create(content);
    lv_label_set_text(state_body, state_label(session_manager().state()));
    lv_obj_set_style_text_color(state_body, state_color(session_manager().state()), 0);

    lv_obj_t * detail_body = lv_label_create(content);
    lv_label_set_long_mode(detail_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_body, LV_PCT(100));
    lv_label_set_text(detail_body, session_manager().statusMessage().c_str());
    lv_obj_set_style_text_color(detail_body, lv_color_hex(0xcccccc), 0);

    lv_obj_t * how_header = lv_label_create(content);
    lv_label_set_text(how_header, "Notes");
    lv_obj_set_style_text_color(how_header, lv_color_hex(0x66aaff), 0);

    lv_obj_t * how_body = lv_label_create(content);
    lv_label_set_long_mode(how_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(how_body, LV_PCT(100));
    lv_label_set_text(how_body,
                       "Wireless only for now (this device's one external USB port is "
                       "normally occupied by the boot rootfs drive). Starts this app's own "
                       "WiFi AP (SSID \"carplay_wifi\"), then a Bluetooth-relayed credential "
                       "handoff -- the phone must already be Bluetooth-paired first from the "
                       "Bluetooth screen. Not yet hardware-tested end to end -- video/audio "
                       "channels aren't implemented yet even once connected.");
    lv_obj_set_style_text_color(how_body, lv_color_hex(0x999999), 0);

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
