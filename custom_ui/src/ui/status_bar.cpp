#include "ui/status_bar.h"

#include <cstdio>
#include <ctime>

#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "ui/theme.h"

namespace ui::status_bar {

namespace {

// Collapses the sidecar's full STATE grammar (see android_auto_screen.cpp's
// own, more detailed parse_status_line()) down to a single yes/no for
// the glyph: lit (accent) only when actively Connected, dim
// (secondary) for every other state -- Idle, mid-handshake, Failed, or
// "sidecar not running at all" (the common case before the AA screen
// has ever been opened, since the shared status poll runs with
// allow_spawn=false by default). A 28px-tall bar glyph has no room for
// the detail android_auto_screen.cpp shows; that screen remains the
// place to see *why* it isn't connected.
bool is_aa_connected(const std::string & line) {
    return line.rfind("STATE Connected", 0) == 0;
}

struct Widgets {
    lv_obj_t * clock_label;
    lv_obj_t * bt_icon;
    lv_obj_t * aa_icon;
};

// 2026-09-05: real hardware bug found via code review -- this bar,
// android_auto_screen.cpp, and home_dashboard.cpp each used to run
// their OWN independent AndroidAutoClient + background poll thread,
// all three hitting the sidecar with STATUS every 500-1000ms -- 3
// sockets, 3 threads, for the exact same piece of information. Now
// reads hal::cached_android_auto_status(), which is backed by ONE
// shared poller (see androidauto_client.h's own comment) -- this
// function itself never blocks (the real socket I/O happens on that
// shared background thread), so no per-bar PollCache/thread is needed
// here anymore at all.
void poll_timer_cb(lv_timer_t * timer) {
    auto * w = static_cast<Widgets *>(lv_timer_get_user_data(timer));

    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(w->clock_label, buf);

    // Bluetooth: live connection status from BluetoothTelemetry
    auto telem = hal::get_telemetry();
    bool bt_connected = hal::shared_handle().fd >= 0 && telem.connected;
    lv_obj_set_style_text_color(w->bt_icon, bt_connected ? theme::accent() : theme::text_secondary(), 0);

    bool aa_ok = is_aa_connected(hal::cached_android_auto_status().status_line);
    lv_obj_set_style_text_color(w->aa_icon, aa_ok ? theme::success() : theme::text_secondary(), 0);
}

void screen_delete_cb(lv_event_t * e) {
    auto * timer = static_cast<lv_timer_t *>(lv_event_get_user_data(e));
    lv_timer_delete(timer);
}

void widgets_delete_cb(lv_event_t * e) {
    delete static_cast<Widgets *>(lv_event_get_user_data(e));
}

}  // namespace

void create(lv_obj_t * scr) {
    lv_obj_t * bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), kHeight);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::surface(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Divider on the TOP edge now -- bar sits at the bottom of the
    // screen, so the seam between it and the rest of the content is
    // above it, not below.
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, theme::surface_border(), 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // montserrat_20, not _14 -- the whole point of doubling kHeight was
    // to make this bar's content actually legible at a glance, not
    // just occupy more empty space.
    lv_obj_t * clock_label = lv_label_create(bar);
    lv_label_set_text(clock_label, "--:--");
    lv_obj_set_style_text_color(clock_label, theme::text_primary(), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);

    lv_obj_t * icons = lv_obj_create(bar);
    lv_obj_remove_style_all(icons);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icons, 16, 0);
    lv_obj_clear_flag(icons, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * aa_icon = lv_label_create(icons);
    lv_label_set_text(aa_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(aa_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(aa_icon, theme::text_secondary(), 0);

    lv_obj_t * bt_icon = lv_label_create(icons);
    lv_label_set_text(bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bt_icon, theme::text_secondary(), 0);

    auto * widgets = new Widgets{clock_label, bt_icon, aa_icon};
    lv_obj_add_event_cb(bar, widgets_delete_cb, LV_EVENT_DELETE, widgets);

    // Runs immediately once (not just on the first 1s tick) so the bar
    // never shows the "--:--" placeholder for a full second after a
    // screen loads.
    lv_timer_t * timer = lv_timer_create(poll_timer_cb, 1000, widgets);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, timer);
    poll_timer_cb(timer);
}

}  // namespace ui::status_bar
