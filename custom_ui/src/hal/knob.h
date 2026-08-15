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
//
// 2026-08-15: also the single point that decides where physical knob
// events go -- local LVGL group navigation (the default, every screen
// except Android Auto) or forwarded into a live AA session's
// InputChannel as real Android KeyEvent taps (KEYCODE_SYSTEM_
// NAVIGATION_UP/DOWN for rotation, KEYCODE_DPAD_CENTER for the push
// button -- see hal/androidauto_client.h's sendKey() and
// androidauto/input_channel.h's own comment for the full keycode
// story). Has to be exactly one consumer of McuInputHal::
// consume_knob_ticks()/get_knob_pressed() -- both are destructive
// reads (ticks reset to 0, matching LVGL's own enc_diff-since-last-
// read contract) -- so androidauto_screen_active() below gates
// ROUTING within this single read callback rather than adding a
// second independent poller that would race the first for the same
// ticks.
#pragma once

#include <atomic>

#include "lvgl.h"
#include "hal/mcu_input.h"

namespace hal {

// Creates an LVGL encoder indev backed by mcu. mcu must already be
// started (McuInputHal::start()) and must outlive the returned indev.
// Caller (main.cpp) still needs to lv_indev_set_group() the result
// once a default group exists.
lv_indev_t * init_knob(McuInputHal & mcu);

// True while ui/android_auto_screen.cpp is the active screen -- set/
// cleared there, alongside its existing client().setVisible() calls
// (same lifecycle, same reasoning: this is about what the physical
// knob should currently be doing, not the AA session's own lifetime,
// which continues regardless via auto-start). While true, the knob
// read callback forwards rotation/push into the live AA session
// instead of driving local LVGL group navigation (which would have
// nothing focusable to navigate on this screen anyway).
std::atomic<bool> & androidauto_screen_active();

}  // namespace hal
