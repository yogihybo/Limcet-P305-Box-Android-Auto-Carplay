// Touch input HAL.
//
// Real hardware finding (2026-08-11): this device's local ADC touch
// controller (previously wired here via LVGL's lv_evdev driver against
// /dev/input/event0) delivers zero touch data on this specific board --
// confirmed directly, MsnCoreApp fully disabled. Real touch coordinates
// are relayed by the Limcet MCU over /dev/ttyHS0 instead -- see
// hal/mcu_input.h and docs/MCU_ADAPTERS.md's "MCU role -- key/status
// events" section for the full capture/analysis trail. This HAL wraps a
// custom LVGL indev backed by that MCU protocol, not evdev.
//
// Takes a McuInputHal& rather than owning one: touch, knob rotation,
// and knob push-button all share the same physical /dev/ttyHS0
// connection (one reader thread, see mcu_input.h), so main.cpp owns a
// single McuInputHal instance and passes it to both hal::init_touch()
// and hal::init_knob().
//
// Hardware-confirmed working (2026-08-11 real device test).
#pragma once

#include "lvgl.h"
#include "hal/mcu_input.h"

namespace hal {

// Creates an LVGL pointer indev backed by mcu. mcu must already be
// started (McuInputHal::start()) and must outlive the returned indev.
lv_indev_t * init_touch(McuInputHal & mcu);

}  // namespace hal
