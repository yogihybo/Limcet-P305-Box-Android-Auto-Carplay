#include "hal/touch.h"

#include <cstdio>

namespace hal {

lv_indev_t * init_touch(const char * event_path) {
    lv_indev_t * indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, event_path);
    if (!indev) {
        std::fprintf(stderr, "hal::init_touch: warning: touch input unavailable at %s, "
                     "continuing display-only\n", event_path);
    }
    return indev;
}

}  // namespace hal
