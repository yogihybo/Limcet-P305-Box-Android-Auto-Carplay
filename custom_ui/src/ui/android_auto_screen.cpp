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

void bluetooth_btn_cb(lv_event_t *) {
    core::navigation::push(ui::create_bluetooth_screen);
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
//
// 2026-08-12: also toggles `content`'s visibility as a whole, not just
// its text -- per explicit request, this screen should show a real
// "connect your phone" instructions view whenever there's no active
// session, and get out of the way once there IS one. `content` uses
// theme::style_card(), which is fully opaque (bg_opa=LV_OPA_COVER) --
// left showing while Connected, it would sit on top of and completely
// hide the AA video hardware layer (see video_visibility.h) that this
// same screen's setVisible(true) call is supposed to be revealing.
void poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<Widgets *>(lv_timer_get_user_data(timer));

    ParsedStatus status = parse_status_line(client().statusLine());
    lv_label_set_text(w->state_label, status.name.c_str());
    lv_obj_set_style_text_color(w->state_label, color_for_state_name(status.name), 0);
    lv_label_set_text(w->detail_label, status.detail.c_str());

    // 2026-08-19: real hardware showed the knob stuck routing to AA
    // (SYSTEM_NAVIGATION/DPAD_CENTER via sendKey()) after the PHONE
    // ended the session (bye-bye) while the user was still sitting on
    // this screen -- androidauto_screen_active() used to only flip on
    // screen create/destroy (see create_android_auto_screen()'s own
    // comment and screen_delete_cb() below), which tracks whether this
    // screen INSTANCE is alive, not whether there's actually a live
    // session to receive those keys. Ticks/presses kept registering in
    // hal/knob.cpp's own log but had no visible effect (nothing on the
    // phone left to receive them), and the knob couldn't drive local
    // LVGL focus (e.g. onto this screen's own back button, now visible
    // again below) to get back out either. Tying this to the same
    // Connected/not-Connected check that already gates content/display
    // visibility fixes both: only route the knob to AA while a session
    // is genuinely showing video.
    hal::androidauto_screen_active().store(status.name == "Connected", std::memory_order_release);

    if (status.name == "Connected") {
        lv_obj_add_flag(w->content, LV_OBJ_FLAG_HIDDEN);
        // 2026-08-15: per explicit request, AA should take over the
        // FULL screen once connected, not just the area underneath
        // this screen's own (now-transparent, see create_android_auto_
        // screen()'s bg_opa comment) body -- AA's own in-app UI has its
        // own exit affordance, so this screen's header/back button
        // don't need to stay visible on top of the video. Hardware-
        // level layer disable (hal::hide_display(), same ARKFB_HIDE_
        // WINDOW_REAL ioctl already proven for the video layer itself)
        // rather than relying solely on LVGL-side transparency, since
        // it sidesteps the LCDC compositor's alpha-blend behavior
        // entirely -- see reverse_camera_screen.cpp's own header
        // comment for the still-open uncertainty that motivates this.
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
    // 2026-08-15: defensive restore -- poll_timer_cb() above only calls
    // hal::show_display() on a Connected->not-Connected transition
    // observed via its own 500ms poll, which never fires again once
    // this screen is torn down. Without this, navigating away (or the
    // app itself exiting) while still Connected would leave the OSD2/
    // LVGL hardware layer permanently disabled -- this whole UI would
    // go dark with no way back short of a reboot.
    if (pair->second->display_hidden) {
        hal::show_display();
    }
    delete pair->second;
    delete pair;
    // 2026-08-12: hide the AA video hardware layer the moment this
    // screen goes away -- see hal::AndroidAutoClient::setVisible()'s
    // own comment. Session/decode keep running in the background
    // regardless (auto-start, see main.cpp's AaAutoStartWatcher); only
    // the hardware layer's visibility is tied to this screen.
    client().setVisible(false);
    // 2026-08-15: pairs with the setVisible(true)/androidauto_screen_
    // active().store(true) below -- the physical knob goes back to
    // driving local LVGL group navigation on whatever screen the user
    // navigates to next.
    hal::androidauto_screen_active().store(false, std::memory_order_release);
}

}  // namespace

lv_obj_t * create_android_auto_screen() {
    lv_obj_t * scr = nullptr;
    theme::create_screen_with_header(&scr, "Android Auto", back_btn_cb);
    // 2026-08-15: found on real hardware -- video decoded correctly,
    // hal::video_layer reported the frame pushed and the layer shown,
    // but nothing ever appeared on screen, still showing this screen's
    // own LVGL content. Root cause: theme::create_screen_with_header()
    // paints an OPAQUE solid background on `scr` itself
    // (lv_obj_set_style_bg_color(scr, bg(), 0), bg_opa defaults to
    // LV_OPA_COVER) -- this screen was covering the ENTIRE 800x480
    // area, including wherever `content` is hidden once Connected, so
    // the video hardware layer underneath (fb4/VIDEO_LAYER2, see
    // hal/video_layer.h's own top comment) had no way to
    // ever show through regardless of it actually being shown at the
    // hardware level. Matches the exact pattern already established
    // and hw-tested for the reverse-camera preview
    // (reverse_camera_screen.cpp's own bg_opa=LV_OPA_TRANSP) -- header/
    // status-bar/back-button are separate child widgets with their own
    // opaque styling, so this only affects the screen's own root fill,
    // not their visibility.
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);

    // 2026-08-12: reveals the AA video hardware layer (if a session is
    // already running -- e.g. auto-started in the background, see
    // main.cpp's AaAutoStartWatcher) the moment this screen is
    // selected, per explicit request: selecting the AA icon should
    // load the video feed directly, not just show a status screen
    // while video stays hidden. No-op (returns false, logged nowhere
    // since it's not an error) if no sidecar/session exists yet --
    // this doesn't start one, connect_btn_cb()/AutoStartCarLink do
    // that; this only ever affects visibility. Whether it's ACTUALLY
    // visible once revealed depends on poll_timer_cb() below hiding
    // this screen's own opaque `content` card once Connected, since
    // that would otherwise sit on top of the video layer regardless of
    // this call.
    client().setVisible(true);
    // 2026-08-19: starts false, not true -- poll_timer_cb() below is
    // now the real, ongoing source of truth for this flag (see its own
    // comment), flipping it true only once status is actually
    // "Connected" and back to false the moment it isn't. Before that
    // first poll (or whenever not connected), this screen's own back
    // button/instructions card ARE meaningful focusable widgets, so the
    // knob needs local LVGL navigation here, not AA routing.
    hal::androidauto_screen_active().store(false, std::memory_order_release);

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

    // "Connect your phone" instructions -- shown whenever there's no
    // active session (poll_timer_cb() hides the WHOLE `content` card,
    // this header included, once Connected). Per explicit request:
    // tapping the AA icon with no phone connected should walk the user
    // through what to do, not just show a bare status line.
    lv_obj_t * instructions_header = lv_label_create(content);
    lv_label_set_text(instructions_header, "Connect your phone");
    theme::style_section_label(instructions_header);

    lv_obj_t * step1 = lv_label_create(content);
    lv_label_set_long_mode(step1, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(step1, LV_PCT(100));
    lv_label_set_text(step1,
                       "1. Pair your phone over Bluetooth first, if you haven't already.");
    theme::style_secondary_text(step1);

    lv_obj_t * bluetooth_btn = lv_button_create(content);
    theme::style_primary_button(bluetooth_btn);
    // Same row-scoped shrink as settings_screen.cpp's own Bluetooth row
    // button and this screen's Connect button below -- style_primary_button()'s
    // full CTA sizing is more than a secondary "go pair a device" link
    // needs here, next to a numbered instruction line rather than
    // standing alone.
    lv_obj_set_style_pad_hor(bluetooth_btn, 14, 0);
    lv_obj_set_style_pad_ver(bluetooth_btn, 8, 0);
    lv_obj_set_style_text_font(bluetooth_btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_min_height(bluetooth_btn, 40, 0);
    lv_obj_add_event_cb(bluetooth_btn, bluetooth_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(core::navigation::focus_group(), bluetooth_btn);
    lv_obj_t * bluetooth_btn_label = lv_label_create(bluetooth_btn);
    lv_label_set_text(bluetooth_btn_label, "Open Bluetooth settings");

    lv_obj_t * step2 = lv_label_create(content);
    lv_label_set_long_mode(step2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(step2, LV_PCT(100));
    lv_label_set_text(step2,
                       "2. Android Auto starts automatically once your phone is detected as "
                       "Auto-capable over Bluetooth -- no action needed. If it doesn't, or "
                       "you'd rather start it now, tap Connect below.");
    theme::style_secondary_text(step2);

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
