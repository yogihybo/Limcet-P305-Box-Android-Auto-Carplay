#include "hal/touch.h"

#include "core/async_worker.h"
#include "hal/androidauto_client.h"
#include "hal/knob.h"

namespace hal {

namespace {

// Own client instance, separate from android_auto_screen.cpp's/
// status_bar.cpp's/knob.cpp's -- allow_spawn is always false for
// sendTouch() (see AndroidAutoClient::sendTouch()'s own comment), so
// this never races anyone else's spawn logic; a dedicated instance
// just keeps this file self-contained rather than reaching into UI
// code from hal/. Same convention as hal/knob.cpp's own instance.
AndroidAutoClient & androidauto_client() {
    static AndroidAutoClient client;
    return client;
}

// 2026-09-03: mcu_touch_read_cb() below is an LVGL indev read callback
// -- it runs on the LVGL main thread, once per touch sample. sendTouch()
// is a real blocking socket call (bounded ~1s SO_RCVTIMEO/SO_SNDTIMEO,
// see androidauto_client.cpp) -- calling it inline here means a wedged
// sidecar freezes the ENTIRE UI (not just AA touch), one read cycle at a
// time, for as long as dragging continues. Route through AsyncWorker
// instead: the LVGL thread only ever pushes a cheap lambda onto a
// mutex-guarded queue, a dedicated worker thread does the actual
// blocking call, in the same DOWN/MOVE/UP order it was generated (see
// core/async_worker.h's own header comment for why per-call detached
// threads aren't safe here).
core::AsyncWorker & touch_forward_worker() {
    static core::AsyncWorker worker;
    return worker;
}

// Edge-detects DOWN/MOVE/UP from get_touch_state()'s level/state
// pressed flag -- same reasoning as knob.cpp's knob_was_pressed(),
// just three states instead of a single press edge (a touch panel
// needs MOVE reported every sample while held, not just once).
bool & touch_was_pressed() {
    static bool was_pressed = false;
    return was_pressed;
}

void mcu_touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));
    McuTouchState state = mcu->get_touch_state();

    // 2026-08-15: same "one consumer, route within this single read
    // callback" reasoning as knob.cpp's mcu_knob_read_cb() -- while the
    // Android Auto screen is active, real touch samples forward into
    // the live AA session as InputChannel TouchReports (via
    // androidauto-sidecar, see hal::AndroidAutoClient::sendTouch()'s
    // own comment for why this replaced the old, dead-on-this-hardware
    // TouchForwarder/evdev design) instead of driving local LVGL
    // widgets -- that screen has no interactive widgets of its own once
    // connected anyway (see android_auto_screen.cpp's own comment on
    // hiding `content`/the OSD2 layer once Connected).
    if (androidauto_screen_active().load(std::memory_order_acquire)) {
        bool was_pressed = touch_was_pressed();
        if (state.pressed && !was_pressed) {
            std::uint32_t x = state.x, y = state.y;
            touch_forward_worker().enqueue([x, y]() {
                androidauto_client().sendTouch(x, y, TouchAction::Down);
            });
        } else if (state.pressed && was_pressed) {
            std::uint32_t x = state.x, y = state.y;
            touch_forward_worker().enqueue([x, y]() {
                androidauto_client().sendTouch(x, y, TouchAction::Move);
            });
        } else if (!state.pressed && was_pressed) {
            std::uint32_t x = state.x, y = state.y;
            touch_forward_worker().enqueue([x, y]() {
                androidauto_client().sendTouch(x, y, TouchAction::Up);
            });
        }
        touch_was_pressed() = state.pressed;

        data->point.x = 0;
        data->point.y = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    touch_was_pressed() = state.pressed;
    data->point.x = state.x;
    data->point.y = state.y;
    data->state = state.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

lv_indev_t * init_touch(McuInputHal & mcu) {
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mcu_touch_read_cb);
    lv_indev_set_driver_data(indev, &mcu);
    lv_timer_t * timer = lv_indev_get_read_timer(indev);
    if (timer) {
        lv_timer_set_period(timer, 10);
    }
    return indev;
}

}  // namespace hal
