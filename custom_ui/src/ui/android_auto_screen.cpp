// Android Auto is deliberately NOT wired into this UI binary yet --
// see Makefile's UI_SRCS comment: src/androidauto/* (the real aasdk-
// backed Session, USB/wireless probes, bw_aap wireless-discovery
// client) needs aasdk/Boost/OpenSSL/libusb/Protobuf, and is only
// linked into the separate androidauto-*-test tools today, not
// `custom_ui` itself (docs/IMPLEMENTATION_PLAN.md Phase 2 is the real
// session integration; still open). So this screen can't show a live
// connection state -- it would be fabricating status this process has
// no way to observe. Matches the settings screen's WiFi row pattern
// (ui/settings_screen.cpp build_basic_tab): informational text about
// how the pipeline actually works today, not a fake live control.
#include "ui/android_auto_screen.h"

#include "core/navigation.h"

namespace ui {

namespace {

void back_btn_cb(lv_event_t *) {
    core::navigation::pop();
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

    lv_obj_t * status_header = lv_label_create(content);
    lv_label_set_text(status_header, "Status");
    lv_obj_set_style_text_color(status_header, lv_color_hex(0x66aaff), 0);

    lv_obj_t * status_body = lv_label_create(content);
    lv_label_set_long_mode(status_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_body, LV_PCT(100));
    lv_label_set_text(status_body,
                       "The real Android Auto session (aasdk control channel, wired USB "
                       "and wireless bw_aap discovery -- see src/androidauto/) isn't linked "
                       "into this UI process yet. It currently only runs standalone, driven "
                       "by the androidauto-*-test tools, while the real session/service "
                       "integration (docs/IMPLEMENTATION_PLAN.md Phase 2) is still open. "
                       "This screen will show live connection state once that lands.");
    lv_obj_set_style_text_color(status_body, lv_color_hex(0xcccccc), 0);

    lv_obj_t * how_header = lv_label_create(content);
    lv_label_set_text(how_header, "How to connect today");
    lv_obj_set_style_text_color(how_header, lv_color_hex(0x66aaff), 0);

    lv_obj_t * how_body = lv_label_create(content);
    lv_label_set_long_mode(how_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(how_body, LV_PCT(100));
    lv_label_set_text(how_body,
                       "Wired: plug a phone into the usb0 port (Accessory-class USB, "
                       "handled by the vendor sink binary). Wireless: pair over Bluetooth "
                       "first from the Bluetooth screen, then the head unit's own WiFi AP "
                       "handshake starts automatically.");
    lv_obj_set_style_text_color(how_body, lv_color_hex(0x999999), 0);

    return scr;
}

}  // namespace ui
