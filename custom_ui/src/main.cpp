// custom_ui entry point.
//
// Phase 1: HAL (src/hal) owns fbdev/evdev setup, core::ScreenManager
// owns the screen stack, ui:: provides screen factories. main() itself
// is now just wiring + the LVGL tick loop. See docs/IMPLEMENTATION_PLAN.md.

#include <cstdio>
#include <unistd.h>
#include "lvgl.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "core/screen_manager.h"
#include "ui/home_screen.h"

int main() {
    lv_init();

    if (!hal::init_display("/dev/fb0")) {
        return 1;
    }
    hal::init_touch("/dev/input/event0");  // non-fatal if unavailable

    core::ScreenManager screens;
    screens.push(ui::create_home_screen);

    std::printf("custom_ui: LVGL initialized, running main loop\n");
    while (true) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
