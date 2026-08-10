// Settings screen -- Phase 3 (docs/IMPLEMENTATION_PLAN.md). Tiered
// Basic/Advanced menu backed by core::ConfigStore (the live
// /data/msncfg/Setting.config layer, factory .ini as fallback seed --
// see core/config_store.h) and, for display adjustments, the real
// /dev/ark_display VDE ioctls (hal/display_ctrl.h).
//
// "Same options as stock, better format" (see IMPLEMENTATION_PLAN.md's
// Phase 3 design principle) -- Basic tier covers the handful of
// settings a daily user touches (language, volume, display, Bluetooth,
// WiFi status); Advanced tier covers the rest of
// docs/SETTINGS_REFERENCE.md's confirmed-live fields, one tap away.
// Fields SETTINGS_REFERENCE.md/project_msnproductinfo_config_exploration
// confirmed dead at runtime (MirroringLinkType, ScreenType as a
// user-settable value) are shown read-only with that note, not
// reimplemented as if they did something.
#pragma once

#include "lvgl.h"

namespace ui {

// Matches core::ScreenManager::ScreenFactory.
lv_obj_t * create_settings_screen();

}  // namespace ui
