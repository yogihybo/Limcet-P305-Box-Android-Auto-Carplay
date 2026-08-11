#include "hal/touch.h"

#include <cstdio>

namespace hal {

namespace {

void mcu_touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuTouchHal *>(lv_indev_get_driver_data(indev));
    McuTouchState state = mcu->get_state();

    data->point.x = state.x;
    data->point.y = state.y;
    data->state = state.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

lv_indev_t * init_touch(const char * mcu_port) {
    // Process-lifetime, intentionally never freed -- same convention as
    // every other process-lifetime singleton in this codebase (e.g.
    // bluetooth_screen.cpp's bt_handle()); this app has no shutdown
    // path today.
    auto * mcu = new McuTouchHal(mcu_port);
    if (!mcu->start()) {
        std::fprintf(stderr, "hal::init_touch: warning: MCU touch input unavailable at %s, "
                     "continuing display-only\n", mcu_port);
        delete mcu;
        return nullptr;
    }

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mcu_touch_read_cb);
    lv_indev_set_driver_data(indev, mcu);
    return indev;
}

}  // namespace hal
