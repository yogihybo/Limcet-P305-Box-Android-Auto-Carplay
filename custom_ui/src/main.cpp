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
#include "core/config_store.h"
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
    // app and nothing filled the gap until now).
    hal::ensure_bluetooth_daemon_running();
    std::printf("custom_ui: bluetooth daemon launch requested\n");

    // 2026-08-12: opens hal::shared_handle() (the one process-wide BT
    // handle -- status_bar.cpp and bluetooth_screen.cpp now use this
    // same one instead of each independently opening their own fd) and
    // attempts to reconnect the last paired device immediately, matching
    // this device's real factory default (FactoryConfig.ini's
    // AutoConnect=1) that custom_ui never actually implemented before --
    // a previously-connected phone stayed unconnected until a user
    // manually opened Settings -> Bluetooth. init_bluetooth() itself
    // retries opening /dev/bw_serial for a couple of seconds, covering
    // blueware's own startup time from the ensure_bluetooth_daemon_running()
    // call just above.
    hal::BluetoothHandle & bt = hal::shared_handle();
    if (bt.fd >= 0) {
        // 2026-08-12: apply the configured Bluetooth name every boot,
        // per request -- previously hal::set_device_name() (AT+NAME=)
        // was only ever called from bluetooth_screen.cpp's Save button,
        // so the name a phone actually saw was whatever was already
        // persisted in the Feasycom module's own NVRAM from some
        // earlier session (stock MsnCoreApp, or blueware's own
        // compiled-in "FSC-CARKIT" default if never set at all) --
        // config_store.h's DeviceName was pure UI decoration until now,
        // never actually reaching the adapter on its own. core::
        // default_store() is the same live ConfigStore the Settings ->
        // Bluetooth screen reads/writes, so a name changed there and
        // saved takes effect on the NEXT boot too, not just
        // immediately via that screen's own Save handler.
        // Fallback "Prado CustomUI" (not stock's "Limcet Box") matches
        // etc/default_settings.conf's own DeviceName -- see that file's
        // comment: kept distinct so a phone doesn't confuse this build
        // with stock firmware on the same physical BT chip/MAC when
        // dual-booting between them.
        std::string btName = core::default_store().get_string("DeviceName", "Prado CustomUI", "BlueTooth");
        if (hal::set_device_name(bt, btName)) {
            std::printf("custom_ui: bluetooth device name set to '%s'\n", btName.c_str());
        } else {
            std::fprintf(stderr, "custom_ui: failed to set bluetooth device name to '%s'\n",
                         btName.c_str());
        }
        hal::auto_reconnect_paired_device(bt);
    }

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
