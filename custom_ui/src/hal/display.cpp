#include "hal/display.h"

#include <cstdio>

namespace hal {

lv_display_t * init_display(const char * fb_path) {
    lv_display_t * disp = lv_linux_fbdev_create();
    if (!disp) {
        std::fprintf(stderr, "hal::init_display: lv_linux_fbdev_create() failed\n");
        return nullptr;
    }
    if (lv_linux_fbdev_set_file(disp, fb_path) != LV_RESULT_OK) {
        std::fprintf(stderr, "hal::init_display: failed to open %s\n", fb_path);
        return nullptr;
    }
    return disp;
}

}  // namespace hal
