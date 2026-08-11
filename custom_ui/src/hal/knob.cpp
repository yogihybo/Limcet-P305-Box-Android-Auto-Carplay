#include "hal/knob.h"

namespace hal {

namespace {

void mcu_knob_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));

    data->enc_diff = static_cast<int16_t>(mcu->consume_knob_ticks());
    data->state = mcu->get_knob_pressed() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

lv_indev_t * init_knob(McuInputHal & mcu) {
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, mcu_knob_read_cb);
    lv_indev_set_driver_data(indev, &mcu);
    return indev;
}

}  // namespace hal
