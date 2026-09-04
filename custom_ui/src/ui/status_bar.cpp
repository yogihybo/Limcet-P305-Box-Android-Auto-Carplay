#include "ui/status_bar.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

#include "core/sized_thread.h"
#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "ui/theme.h"

namespace ui::status_bar {

namespace {

// Process-lifetime singleton, same pattern as every other HAL handle
// in this codebase (settings_screen.cpp's display_handle()) --
// connected lazily once, shared by every screen's status bar instance
// rather than one per screen visit, so navigating around the app
// doesn't repeatedly reconnect the sidecar socket.
//
// Deliberately a SEPARATE hal::AndroidAutoClient from android_auto_screen.cpp's
// own -- see status_bar.h's top comment for why a second socket
// connection is the simpler, more decoupled choice here over threading
// a shared client instance across translation units.
hal::AndroidAutoClient & aa_client() {
    static hal::AndroidAutoClient c;
    return c;
}

// Collapses the sidecar's full STATE grammar (see android_auto_screen.cpp's
// own, more detailed parse_status_line()) down to a single yes/no for
// the glyph: lit (accent) only when actively Connected, dim
// (secondary) for every other state -- Idle, mid-handshake, Failed, or
// "sidecar not running at all" (the common case before the AA screen
// has ever been opened, since this bar polls with allow_spawn=false).
// A 28px-tall bar glyph has no room for the detail android_auto_screen.cpp
// shows; that screen remains the place to see *why* it isn't connected.
bool is_aa_connected(const std::string & line) {
    return line.rfind("STATE Connected", 0) == 0;
}

// 2026-09-04: real hardware bug -- poll_timer_cb() used to call
// aa_client().statusLine(false) directly, inline, on the LVGL main
// thread every 1000ms, on EVERY screen (this bar is persistent, not
// AA-specific). That's a synchronous AndroidAutoClient socket call
// bounded by SO_RCVTIMEO/SO_SNDTIMEO=1s x up to 2 attempts (~2s worst
// case) -- since that exceeds the 1000ms poll period, a wedged sidecar
// could freeze the entire UI continuously, on any screen. Same bug
// class as mcu_input.cpp's reader-thread freeze and
// android_auto_screen.cpp's own poll_timer_cb() (see that file's
// PollCache for the fuller version of this same comment) -- fixed the
// same way: a background thread owns the blocking call, the LVGL timer
// only ever reads a cached atomic bool.
struct PollCache {
    std::atomic<bool> aa_ok{false};
    std::atomic<bool> stop{false};
};

void poll_background_loop(PollCache * cache) {
    while (!cache->stop.load(std::memory_order_acquire)) {
        bool ok = is_aa_connected(aa_client().statusLine(/*allow_spawn=*/false));
        cache->aa_ok.store(ok, std::memory_order_relaxed);
        for (int i = 0; i < 10 && !cache->stop.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    // Background thread owns cache's lifetime once started -- the LVGL
    // side only ever sets stop=true, never touches cache again after
    // that (widgets_delete_cb below), so it's safe for this thread to
    // free it once its own loop actually exits.
    delete cache;
}

struct Widgets {
    lv_obj_t * clock_label;
    lv_obj_t * bt_icon;
    lv_obj_t * aa_icon;
    PollCache * cache;
};

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

    bool aa_ok = w->cache->aa_ok.load(std::memory_order_relaxed);
    lv_obj_set_style_text_color(w->aa_icon, aa_ok ? theme::success() : theme::text_secondary(), 0);
}

void screen_delete_cb(lv_event_t * e) {
    auto * timer = static_cast<lv_timer_t *>(lv_event_get_user_data(e));
    lv_timer_delete(timer);
}

void widgets_delete_cb(lv_event_t * e) {
    auto * w = static_cast<Widgets *>(lv_event_get_user_data(e));
    // Signals the background poll loop to stop; it frees `cache` itself
    // once its own loop notices -- see poll_background_loop()'s own
    // comment on this ownership split.
    w->cache->stop.store(true, std::memory_order_release);
    delete w;
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

    auto * cache = new PollCache();
    core::SizedThread(core::kDefaultThreadStackSize, poll_background_loop, cache).detach();

    auto * widgets = new Widgets{clock_label, bt_icon, aa_icon, cache};
    lv_obj_add_event_cb(bar, widgets_delete_cb, LV_EVENT_DELETE, widgets);

    // Runs immediately once (not just on the first 1s tick) so the bar
    // never shows the "--:--" placeholder for a full second after a
    // screen loads. The AA icon itself may still show its prior/default
    // (dim) state for a moment until poll_background_loop()'s first
    // real result lands in the cache -- never blocks either way.
    lv_timer_t * timer = lv_timer_create(poll_timer_cb, 1000, widgets);
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, timer);
    poll_timer_cb(timer);
}

}  // namespace ui::status_bar
