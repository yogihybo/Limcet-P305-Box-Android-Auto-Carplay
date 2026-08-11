// Persistent top status bar, Android-Auto-style -- every screen except
// reverse_camera_screen.cpp (see its own top comment: forcing an
// opaque strip over a transparent hardware-video overlay would defeat
// the whole point of that screen) mounts one of these, right after
// creating its own lv_obj_t* screen and before building the rest of
// its content.
//
// Design call: this is a SEPARATE component from theme::create_screen_with_header()
// rather than folded into it, even though every non-home screen already
// calls create_screen_with_header() and could pick this up "for free"
// by doing so (and does -- see theme.cpp). Kept as its own header/.cpp
// because it needs hal::BluetoothHandle and hal::AndroidAutoClient,
// neither of which theme.h otherwise depends on; keeping that HAL
// knowledge out of theme.cpp keeps the base theming file a pure
// LVGL-styling concern, same spirit as this codebase's existing "one
// concern per file" HAL layering (theme.cpp doesn't know about
// /dev/bw_serial or the sidecar socket protocol, status_bar.cpp does).
//
// What's real vs. stubbed here, stated plainly (this codebase's own
// convention -- see e.g. hal/bluetooth.h's top comment):
//   - Clock: real. time()/localtime()/strftime() against the Linux
//     system clock, updated once a second by an lv_timer_create().
//   - Bluetooth glyph: real, but narrower than "connected to a phone".
//     It reflects whether /dev/bw_serial opened successfully (hardware
//     present/reachable), not live pairing/connection state -- the
//     AT-response grammar for that (+HFPSTAT=...) was never confirmed
//     against real captured traffic (see hal/bluetooth.h's top
//     comment), so this deliberately doesn't claim more than the
//     honest, confirmed signal: adapter reachable or not.
//   - Android Auto glyph: real, wired to hal::AndroidAutoClient's
//     sidecar protocol, but polled with allow_spawn=false (see
//     androidauto_client.h) so a status glyph visible on every screen
//     never itself starts the aasdk-backed sidecar process -- it only
//     lights up once android_auto_screen.cpp (or anything else that
//     calls statusLine()/requestConnect() with spawning allowed) has
//     already started it. Uses its OWN AndroidAutoClient instance (a
//     second socket connection to the sidecar, separate from
//     android_auto_screen.cpp's) rather than sharing one across
//     translation units -- see sidecars/androidauto/main.cpp, which
//     already accepts one thread per connection and now expects two.
#pragma once

#include "lvgl.h"

namespace ui::status_bar {

// Total height in pixels, including its bottom divider. Callers that
// otherwise anchor content/header elements to the literal top of the
// screen (theme::add_back_button's TOP_LEFT offset, theme::add_title's
// TOP_MID offset, home_screen.cpp's own title) need to add this to
// their own top offset so nothing sits underneath the bar -- see
// theme.cpp's create_screen_with_header() and home_screen.cpp for the
// two places that already do this.
constexpr int32_t kHeight = 28;

// Creates the bar as a child of `scr`, pinned LV_ALIGN_TOP_MID at
// (0, 0). Safe to call on any screen; owns a small lv_timer_t tied to
// `scr`'s LV_EVENT_DELETE for cleanup, same convention as
// android_auto_screen.cpp's poll_timer_cb.
void create(lv_obj_t * scr);

}  // namespace ui::status_bar
