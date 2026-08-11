#include "hal/touch.h"

namespace hal {

namespace {

void mcu_touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));
    McuTouchState state = mcu->get_touch_state();

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
    return indev;
}

}  // namespace hal
