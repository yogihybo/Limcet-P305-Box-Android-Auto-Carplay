// Touch input HAL. Wraps LVGL's lv_evdev driver against the ARK1680
// resistive touch controller's evdev node -- see docs/ARCHITECTURE.md's
// "Touch input" section. Plain evdev, no Qt/QWS env-var gating needed
// (that was a stock-app-specific requirement, not a hardware one).
#pragma once

#include "lvgl.h"

namespace hal {

// Returns nullptr on failure (device missing) -- display-only
// operation should still be possible without touch, not fatal.
lv_indev_t * init_touch(const char * event_path = "/dev/input/event0");

}  // namespace hal
