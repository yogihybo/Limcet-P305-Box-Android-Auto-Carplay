// Launcher / home screen -- Phase 5 (docs/IMPLEMENTATION_PLAN.md). Root
// screen of the app (main.cpp's first ScreenManager::push()); a grid of
// tiles for every real destination this app has today.
#pragma once

#include "lvgl.h"

namespace ui {

// Creates and returns a new home screen object (matches
// core::ScreenManager::ScreenFactory).
lv_obj_t * create_home_screen();

}  // namespace ui
