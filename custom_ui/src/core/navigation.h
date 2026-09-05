// Tiny global accessor letting screen widgets (button event callbacks)
// drive core::ScreenManager::push()/pop() without every screen factory
// needing to carry a captured ScreenManager* -- ScreenManager::ScreenFactory
// is deliberately a plain function pointer (see screen_manager.h), so
// there's no user-data slot on the factory itself to smuggle one
// through.
//
// Safe because LVGL (and therefore every button event callback) only
// ever runs on the single thread driving lv_timer_handler() -- same
// thread that owns the core::ScreenManager instance in main.cpp, see
// reverse_gear_watcher.h's comment on why ScreenManager must not be
// touched from any other thread. This header does not add any new
// threading concern, just a way for same-thread callbacks to reach
// the one ScreenManager instance main() already owns.
#pragma once

#include "lvgl.h"
#include "core/screen_manager.h"

namespace core::navigation {

// Called once from main.cpp after constructing the ScreenManager.
void init(ScreenManager & manager);

// Thin wrappers over the registered ScreenManager. No-ops (with a
// stderr warning) if init() was never called -- shouldn't happen in
// practice, but screens shouldn't crash the process over it.
void push(ScreenManager::ScreenFactory factory);
void replace(ScreenManager::ScreenFactory factory);
void pop();
size_t depth();

// 2026-09-05: real hardware bug found via code review -- this used to
// return ONE process-wide lv_group_t every screen piled its own
// widgets onto. push() never deletes the screen underneath (kept alive
// for a future pop()), and LVGL only removes an object from its group
// when that object is deleted -- so a screen's widgets stayed
// registered in this shared group forever, even after being buried by
// a push(), letting the knob's rotary focus land on (and activate)
// widgets belonging to a screen the user can no longer see. Now
// delegates to core::ScreenManager::current_group() -- each screen
// gets its OWN dedicated group (created in push()/replace(), rebound
// onto the knob indev immediately, freed when that screen is later
// torn down), so only the screen actually on top is ever reachable.
// Screens still add their own focusable widgets to this via
// lv_group_add_obj(focus_group(), obj) exactly as before, same
// "reach the current instance without a captured pointer" rationale
// as push()/pop() above -- only what this returns changed, not how
// it's used.
lv_group_t * focus_group();

}  // namespace core::navigation
