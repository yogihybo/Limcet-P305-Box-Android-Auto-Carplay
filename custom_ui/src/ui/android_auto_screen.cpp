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
#include "hal/bluetooth.h"
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

// 2026-08-20: no longer sends "CONNECT" to the sidecar (a no-op there
// now -- see sidecars/androidauto/main.cpp's own protocol comment).
// Bluetooth connectivity is custom_ui's own job (hal::bluetooth.cpp's
// aa_profile_server_loop()) -- when "Auto-start phone projection"
// (AutoStartCarLink) is off, a connected phone's fd sits stashed there
// waiting for exactly this tap. hal::start_pending_aa_connection() is
// always safe to call: it's a no-op (returns false) if no phone has
// connected over the AA Bluetooth profile yet, same as the old
// requestConnect() was a no-op if the sidecar was unreachable.
void connect_btn_cb(lv_event_t *) {
    if (!hal::start_pending_aa_connection()) {
        hal::BluetoothHandle & h = hal::shared_handle();
        hal::auto_reconnect_paired_device(h);
    }
}

void bluetooth_btn_cb(lv_event_t *) {
    staging_ui::navigate_to(staging_ui::NavDestination::Bluetooth);
}

// 2026-08-19: real gap found on hardware -- once a session backgrounds
// itself (phone's own in-app exit/back control, see
// video_channel.cpp's onVideoFocusRequest() comment), this screen's
// content card comes back per poll_timer_cb()'s own showingVideo logic
// below, but it always showed the NOT-YET-CONNECTED copy ("Ready to
// connect" / "Connect (Wireless)") -- tapping Connect while already
// Connected is at best a confusing no-op (requestConnect() just
// restarts the whole handshake) and at worst disruptive. This sends
// "RESUME" instead, asking the phone to grant PROJECTED focus back --
// see AndroidAutoClient::requestResumeVideo()'s own comment.
void resume_btn_cb(lv_event_t *) {
    client().requestResumeVideo();
}

struct Widgets {
    // The whole instructions/status card -- see poll_timer_cb()'s
    // comment on why this gets hidden entirely (not just its
    // contents) once connected.
    lv_obj_t * content;
    lv_obj_t * state_label;
    lv_obj_t * detail_label;
    lv_obj_t * title;
    lv_obj_t * subtitle;
    lv_obj_t * cta_btn;
    lv_obj_t * cta_label;
    // Tracks whether hal::hide_display() has already been called, so
    // poll_timer_cb() only issues the ioctl on an actual Connected/
    // not-Connected transition rather than every 500ms tick.
    bool display_hidden = false;
    // Tracks which copy/callback the CTA button currently shows, same
    // once-per-transition reasoning as display_hidden above -- see
    // poll_timer_cb()'s own comment.
    bool showing_resume = false;
    // 2026-08-20: same once-per-transition reasoning, for the
    // not-yet-connected state's own copy -- see poll_timer_cb()'s
    // handling of hal::has_pending_aa_connection() below.
    bool showing_pending_ready = false;
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

    // 2026-08-19: Connected alone isn't the right condition to gate
    // fb0/knob/touch on -- Android Auto's own in-app exit/back control
    // sends a real VideoFocusRequestNotification(mode=NATIVE), which
    // per real AA behavior (researched, not guessed -- see
    // video_channel.cpp's onVideoFocusRequest() comment) means "give
    // focus back to your native UI," NOT a disconnect. The session
    // stays fully Connected in the background the whole time -- real
    // hardware showed this exact case (bye-bye never sent, session
    // still Connected) leaving the panel stuck on a frozen AA video
    // frame with fb0 never switched back. Querying videoFocusNative()
    // (see hal/androidauto_client.h / androidauto/video_visibility.h)
    // alongside Connected catches this: only route the knob/touch to
    // AA, hide fb0, and hide this screen's own content card while a
    // session is BOTH Connected AND the phone still wants PROJECTED
    // focus -- i.e. actually showing video.
    bool connected = status.name == "Connected";
    bool nativeFocus = connected && client().videoFocusNative();
    bool showingVideo = connected && !nativeFocus;
    hal::androidauto_screen_active().store(showingVideo, std::memory_order_release);

    if (showingVideo) {
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

    // Backgrounded (Connected but NATIVE focus) needs a "Resume" CTA,
    // not the not-yet-connected "Connect (Wireless)" one -- see
    // resume_btn_cb()'s own comment. Only touched on an actual
    // transition, same reasoning as display_hidden above.
    if (nativeFocus && !w->showing_resume) {
        lv_label_set_text(w->title, "Android Auto is running in the background");
        lv_label_set_text(w->subtitle, "The phone switched away from the projected view. Tap Resume to bring it back to the screen.");
        lv_label_set_text(w->cta_label, "Resume");
        lv_obj_remove_event_cb(w->cta_btn, connect_btn_cb);
        lv_obj_add_event_cb(w->cta_btn, resume_btn_cb, LV_EVENT_CLICKED, nullptr);
        w->showing_resume = true;
    } else if (!nativeFocus && w->showing_resume) {
        lv_label_set_text(w->title, "Ready to connect");
        lv_label_set_text(w->subtitle, "Pair phone via Bluetooth to begin wireless session.");
        lv_label_set_text(w->cta_label, "Connect (Wireless)");
        lv_obj_remove_event_cb(w->cta_btn, resume_btn_cb);
        lv_obj_add_event_cb(w->cta_btn, connect_btn_cb, LV_EVENT_CLICKED, nullptr);
        w->showing_resume = false;
        w->showing_pending_ready = false;  // re-evaluated by the block below on the next tick
    }

    // 2026-08-20: while sitting in the plain not-yet-connected state
    // (not nativeFocus/backgrounded, not showing the Resume CTA above),
    // distinguish "no phone connected yet" from "a phone connected over
    // Bluetooth and is waiting for you to tap Connect" -- this is the
    // whole point of the "Auto-start phone projection" setting being
    // off (see hal::has_pending_aa_connection()'s own header comment):
    // without this, the screen would look identical in both cases and
    // the user would have no idea a session is actually ready to start.
    // Only touched on an actual transition, same reasoning as
    // display_hidden/showing_resume above -- has_pending_aa_connection()
    // is a cheap mutex-guarded bool check either way, but no reason to
    // call lv_label_set_text() every 500ms tick when nothing changed.
    if (!connected && !nativeFocus) {
        bool pendingReady = hal::has_pending_aa_connection();
        if (pendingReady && !w->showing_pending_ready) {
            lv_label_set_text(w->title, "Phone connected");
            lv_label_set_text(w->subtitle, "Android Auto is ready -- tap Connect to start.");
            w->showing_pending_ready = true;
        } else if (!pendingReady && w->showing_pending_ready) {
            lv_label_set_text(w->title, "Ready to connect");
            lv_label_set_text(w->subtitle, "Pair phone via Bluetooth to begin wireless session.");
            w->showing_pending_ready = false;
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

    // 2. Main Content Card matching Home Dashboard geometry
    lv_obj_t * content = lv_obj_create(scr);
    staging_ui::theme::style_card(content);
    lv_obj_set_pos(content, staging_ui::theme::kRailWidth + 16, 8);
    lv_obj_set_size(content, 800 - (staging_ui::theme::kRailWidth + 32), 464);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, 32, 0);

    // Title & Instructions
    lv_obj_t * title = lv_label_create(content);
    lv_label_set_text(title, "Ready to connect");
    lv_obj_set_style_text_font(title, &lv_font_roboto_28, 0);
    lv_obj_set_style_text_color(title, staging_ui::theme::text_primary(), 0);

    lv_obj_t * subtitle = lv_label_create(content);
    lv_label_set_text(subtitle, "Pair phone via Bluetooth to begin wireless session.");
    lv_obj_set_style_text_font(subtitle, &lv_font_roboto_14, 0);
    lv_obj_set_style_text_color(subtitle, staging_ui::theme::text_secondary(), 0);

    // Connect (Wireless) CTA Button
    lv_obj_t * connect_btn = lv_button_create(content);
    lv_obj_remove_style_all(connect_btn);
    lv_obj_set_size(connect_btn, 360, 56);
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

    if (core::navigation::focus_group()) {
        lv_group_add_obj(core::navigation::focus_group(), connect_btn);
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

    auto * widgets = new Widgets{content, state_body, detail_body, title, subtitle, connect_btn, connect_label};
    lv_timer_t * timer = lv_timer_create(poll_timer_cb, 500, widgets);
    auto * pair = new std::pair<lv_timer_t *, Widgets *>(timer, widgets);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, pair);

    return scr;
}

}  // namespace ui
