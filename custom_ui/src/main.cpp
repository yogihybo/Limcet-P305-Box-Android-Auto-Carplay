// custom_ui entry point.
//
// Phase 1: HAL (src/hal) owns fbdev/evdev setup, core::ScreenManager
// owns the screen stack, ui:: provides screen factories. main() itself
// is now just wiring + the LVGL tick loop. See docs/IMPLEMENTATION_PLAN.md.

#include <cstdio>
#include <ctime>
#include <unistd.h>
#include "lvgl.h"
#include "hal/display.h"
#include "hal/knob.h"
#include "hal/mcu_input.h"
#include "hal/touch.h"
#include "core/navigation.h"
#include "core/screen_manager.h"
#include "ui/home_screen.h"

namespace {

double monotonic_seconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

}  // namespace

int main() {
    std::printf("custom_ui: starting, lv_init()...\n");
    lv_init();
    std::printf("custom_ui: lv_init() done\n");

    if (!hal::init_display("/dev/fb0")) {
        std::fprintf(stderr, "custom_ui: hal::init_display() failed, exiting\n");
        return 1;
    }
    std::printf("custom_ui: display initialized\n");

    // Process-lifetime, intentionally never freed -- same convention as
    // every other process-lifetime singleton in this codebase. Touch,
    // knob rotation, and knob push-button all share this one
    // /dev/ttyHS0 connection -- see hal/mcu_input.h for why (one reader
    // thread per fd).
    static hal::McuInputHal mcu_input("/dev/ttyHS0");
    bool mcu_ok = mcu_input.start();
    std::printf("custom_ui: MCU input (touch/knob) %s\n", mcu_ok ? "started" : "unavailable");

    lv_indev_t * touch = mcu_ok ? hal::init_touch(mcu_input) : nullptr;
    std::printf("custom_ui: touch %s\n", touch ? "initialized" : "unavailable (continuing without it)");

    lv_indev_t * knob = mcu_ok ? hal::init_knob(mcu_input) : nullptr;
    if (knob) {
        lv_indev_set_group(knob, core::navigation::focus_group());
    }
    std::printf("custom_ui: knob %s\n", knob ? "initialized" : "unavailable (continuing without it)");

    core::ScreenManager screens;
    core::navigation::init(screens);  // lets Settings/Bluetooth screens
                                       // push/pop without a captured
                                       // ScreenManager* -- see core/navigation.h
    screens.push(ui::create_home_screen);
    std::printf("custom_ui: home screen pushed\n");

    std::printf("custom_ui: LVGL initialized, running main loop\n");

    // Heartbeat: prints actual loop iterations/sec once a second. This
    // is a direct empirical check of whether usleep(5000) is really
    // sleeping ~5ms on this device's kernel (see the CPU-usage
    // discussion in project notes) -- if this prints a number far
    // above ~200/sec, usleep() is returning far faster than requested
    // (a real, separate finding from the display-visibility issue).
    long iterations = 0;
    double last_report = monotonic_seconds();

    while (true) {
        lv_timer_handler();
        usleep(5000);

        ++iterations;
        double now = monotonic_seconds();
        double elapsed = now - last_report;
        if (elapsed >= 1.0) {
            std::printf("custom_ui: heartbeat -- %.1f loop iterations/sec over the last %.2fs\n",
                        static_cast<double>(iterations) / elapsed, elapsed);
            iterations = 0;
            last_report = now;
        }
    }

    return 0;
}
