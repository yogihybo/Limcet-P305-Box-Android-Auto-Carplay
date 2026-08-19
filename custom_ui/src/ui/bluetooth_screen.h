// Bluetooth pairing/device menu -- Phase 3 (docs/IMPLEMENTATION_PLAN.md).
// Backed by hal/bluetooth.h and native Linux BlueZ 5.66 (bluetoothd / hci0).
#pragma once

#include "lvgl.h"

namespace ui {

// Matches core::ScreenManager::ScreenFactory.
lv_obj_t * create_bluetooth_screen();

}  // namespace ui
