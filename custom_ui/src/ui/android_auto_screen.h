// Informational Android Auto screen -- see android_auto_screen.cpp's
// top comment for why this is status text, not a live session view.
#pragma once

#include "lvgl.h"

namespace ui {

// Creates and returns a new Android Auto info screen (matches
// core::ScreenManager::ScreenFactory).
lv_obj_t * create_android_auto_screen();

}  // namespace ui
