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

// The single lv_group_t the knob's LVGL encoder indev navigates (see
// hal/knob.h) -- lazily created on first call. Screens add their own
// focusable widgets to this via lv_group_add_obj(focus_group(), obj)
// as they build themselves, same "reach the one shared instance
// without a captured pointer" rationale as push()/pop() above. LVGL
// automatically removes an object from its group when the object
// itself is deleted (ScreenManager::pop() deleting the outgoing
// screen's objects), so there's no manual cleanup needed when
// navigating between screens.
lv_group_t * focus_group();

}  // namespace core::navigation
