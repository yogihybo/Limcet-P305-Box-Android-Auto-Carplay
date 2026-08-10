// Reversing-camera screen. Matches the ui::create_home_screen()
// factory-function convention (core::ScreenManager::ScreenFactory is a
// plain function pointer, no captured state) -- see
// src/core/screen_manager.h and src/ui/home_screen.cpp.
#pragma once

#include "lvgl.h"

namespace ui {

// Deliberately near-empty / transparent-background screen -- see
// reverse_camera_screen.cpp's top comment for why. This does NOT
// render camera pixels itself; the hardware video layer is switched
// on/off independently by the kernel (docs/ARCHITECTURE.md's
// "Reversing camera" section). This screen only supplies minimal
// overlay chrome (a label, a way back) and exists as the thing
// core::ScreenManager pushes/pops when reverse gear engages/
// disengages -- see main.cpp's reverse-gear listener thread.
lv_obj_t * create_reverse_camera_screen();

}  // namespace ui
