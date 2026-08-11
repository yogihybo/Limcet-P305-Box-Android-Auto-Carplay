// custom_ui entry point.
//
// Phase 1: HAL (src/hal) owns fbdev/evdev setup, core::ScreenManager
// owns the screen stack, ui:: provides screen factories. main() itself
// is now just wiring + the LVGL tick loop. See docs/IMPLEMENTATION_PLAN.md.

#include <cstdio>
#include <unistd.h>
#include "lvgl.h"
#include "hal/bluetooth.h"
#include "hal/display.h"
#include "hal/knob.h"
#include "hal/mcu_input.h"
#include "hal/touch.h"
#include "core/navigation.h"
#include "core/screen_manager.h"
#include "ui/home_screen.h"
#include "ui/theme.h"

int main() {
    std::printf("custom_ui: starting, lv_init()...\n");
    lv_init();
    std::printf("custom_ui: lv_init() done\n");

    lv_display_t * disp = hal::init_display("/dev/fb0");
    if (!disp) {
        std::fprintf(stderr, "custom_ui: hal::init_display() failed, exiting\n");
        return 1;
    }
    std::printf("custom_ui: display initialized\n");

    // Dark/accent theme for every default-styled widget (buttons,
    // switches, sliders, dropdowns, tabview) -- must run before any
    // screen is created, see ui/theme.h.
    ui::theme::init(disp);
    std::printf("custom_ui: theme applied\n");

    // Starts /usr/bin/blueware (see hal/bluetooth.h) as early as
    // possible -- nothing else on this device auto-starts it (stock
    // firmware's MsnCoreApp did, at runtime; custom_ui replaces that
    // app and nothing filled the gap until now). Fire-and-forget, not
    // awaited here: hal::init_bluetooth() (called lazily by the first
    // screen that needs it -- see bluetooth_screen.cpp/status_bar.cpp)
    // retries opening /dev/bw_serial for a couple of seconds on its
    // own, which is what actually covers the daemon's startup time;
    // this call just gives it a head start so that retry loop is
    // usually a no-op by the time anything needs it.
    hal::ensure_bluetooth_daemon_running();
    std::printf("custom_ui: bluetooth daemon launch requested\n");

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

    // Per-iteration heartbeat logging (iterations/sec once a second)
    // was removed here -- it was a one-time diagnostic for a real bug
    // (LV_MEM_SIZE too small -> lv_malloc() NULL -> the default assert
    // handler's while(1) spin, which looked like a hang; see
    // lv_conf.h's LV_MEM_SIZE comment), confirmed fixed and hardware-
    // tested. Left logging on afterward, it just drowned out real
    // error output on every boot.
    while (true) {
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
