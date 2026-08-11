// Touch input HAL.
//
// Real hardware finding (2026-08-11): this device's local ADC touch
// controller (previously wired here via LVGL's lv_evdev driver against
// /dev/input/event0) delivers zero touch data on this specific board --
// confirmed directly, MsnCoreApp fully disabled. Real touch coordinates
// are relayed by the Limcet MCU over /dev/ttyHS0 instead -- see
// hal/mcu_touch.h and docs/MCU_ADAPTERS.md's "MCU role -- key/status
// events" section for the full capture/analysis trail. This HAL wraps a
// custom LVGL indev backed by that MCU protocol, not evdev.
//
// Safe to open /dev/ttyHS0 exclusively here: custom_ui and MsnCoreApp
// are never run concurrently (the existing /dev/fb0 handoff model,
// see scripts/run_on_device.sh), and this is a plain userspace serial
// reader -- no kernel changes, nothing that touches MsnCoreApp's own
// operation when it's the one running.
//
// NOT YET HARDWARE-TESTED as an actual input source for this UI.
#pragma once

#include "lvgl.h"
#include "hal/mcu_touch.h"

namespace hal {

// Starts the MCU touch reader and creates an LVGL pointer indev backed
// by it. Returns nullptr (non-fatal, display-only operation should
// still be possible) if the MCU port can't be opened.
lv_indev_t * init_touch(const char * mcu_port = "/dev/ttyHS0");

}  // namespace hal
