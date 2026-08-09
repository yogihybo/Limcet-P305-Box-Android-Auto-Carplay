// Placeholder home screen -- Phase 0/1 milestone content only. Real
// launcher/app-switcher work is Phase 5 (see docs/IMPLEMENTATION_PLAN.md).
#pragma once

#include "lvgl.h"

namespace ui {

// Creates and returns a new home screen object (matches
// core::ScreenManager::ScreenFactory).
lv_obj_t * create_home_screen();

}  // namespace ui
