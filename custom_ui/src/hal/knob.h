// Rotary knob + push-button input HAL. Wraps a custom LVGL encoder
// indev backed by hal::McuInputHal -- see mcu_input.h for the confirmed
// CMD 0x02 protocol (rotation direction codes, push-button state) this
// is built against.
//
// An LVGL encoder indev needs an lv_group_t of focusable objects to
// navigate -- see core/navigation.h for where the shared default group
// is created and how screens register their own focusable widgets into
// it.
//
// NOT YET hardware-tested as an LVGL input source -- built directly
// against the protocol capture/analysis in mcu_input.h and
// docs/MCU_ADAPTERS.md.
#pragma once

#include "lvgl.h"
#include "hal/mcu_input.h"

namespace hal {

// Creates an LVGL encoder indev backed by mcu. mcu must already be
// started (McuInputHal::start()) and must outlive the returned indev.
// Caller (main.cpp) still needs to lv_indev_set_group() the result
// once a default group exists.
lv_indev_t * init_knob(McuInputHal & mcu);

}  // namespace hal
