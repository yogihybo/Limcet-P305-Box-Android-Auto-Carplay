// custom_ui entry point.
//
// Phase 1: HAL (src/hal) owns fbdev/evdev setup, core::ScreenManager
// owns the screen stack, ui:: provides screen factories. main() itself
// is now just wiring + the LVGL tick loop. See docs/IMPLEMENTATION_PLAN.md.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include "lvgl.h"
#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "hal/display.h"
#include "hal/knob.h"
#include "hal/mcu_input.h"
#include "hal/touch.h"
#include "core/config_store.h"
#include "core/navigation.h"
#include "core/screen_manager.h"
#include "ui/android_auto_screen.h"
#include "ui/home_screen.h"
#include "ui/theme.h"

namespace {

// 2026-08-12: auto-starts the wireless Android Auto session the moment
// blueware reports a nearby phone as Android-Auto-capable, instead of
// requiring the user to manually open the Android Auto screen first.
//
// Trigger: a real "+AAPDEV=<mac><sep><name>" broadcast on /dev/bw_serial
// (see hal/bluetooth.h's top comment -- confirmed live, only ever seen
// when a real AA-capable phone was nearby). This is blueware's own
// AAP_ENABLE=1 feature (see etc/blueware-bw121.properties) running the
// same Bluetooth SDP query against bonded devices Google's own AA app
// ("gearhead") does -- checking for the well-known Android Auto
// Wireless service UUID 4de17a00-52cb-11e6-bdf4-0800200c9a66, which
// every phone with the AA app installed publishes in its own SDP
// records. This device has no BlueZ stack to run that query itself
// (see hal/bluetooth.h's architecture comment), so +AAPDEV= is the
// only window into that result -- not a guess, blueware is reporting a
// real SDP-UUID match on our behalf.
//
// The broadcast callback (on_broadcast()) runs on hal::bluetooth's
// background reader thread and must stay fast/non-blocking (see
// hal::watch_bluetooth_broadcasts()'s own contract) -- it just records
// "a trigger is pending" and returns. The actual work (a blocking
// socket call to androidauto-sidecar) happens on this class's own
// dedicated thread instead.
class AaAutoStartWatcher {
public:
    // Debounced per-device: the same phone re-broadcasting +AAPDEV=
    // repeatedly (observed live -- AAPSTAT/AAPDEV can cycle several
    // times around one real detection) only triggers once, not once
    // per broadcast. A DIFFERENT device appearing (or this device
    // reappearing after another device took over the "last triggered"
    // slot) triggers again -- androidauto::WirelessSessionManager::start()
    // is itself safe to call repeatedly (restarts a fresh attempt,
    // same as a manual "Retry"), so re-triggering isn't unsafe, just
    // wasteful, which this debounce avoids for the common case.
    void on_broadcast(const std::string & line) {
        constexpr const char * kPrefix = "+AAPDEV=";
        if (line.rfind(kPrefix, 0) != 0) return;
        std::string entry = line.substr(std::strlen(kPrefix));
        std::string mac, name;
        std::string device_id = hal::split_mac_and_name(entry, mac, name) ? mac : entry;

        std::lock_guard<std::mutex> lock(mtx_);
        if (device_id == last_triggered_id_) return;
        last_triggered_id_ = device_id;
        pending_name_ = name;
        pending_.store(true, std::memory_order_release);
    }

    // Runs for the process's whole lifetime, same "no shutdown path"
    // convention as every other background loop in this codebase.
    void run() {
        for (;;) {
            if (pending_.exchange(false, std::memory_order_acq_rel)) {
                std::string name;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    name = pending_name_;
                }
                std::printf("custom_ui: +AAPDEV= detected ('%s') -- auto-starting wireless "
                            "Android Auto\n", name.c_str());
                // requestConnect() spawns androidauto-sidecar itself if
                // it isn't already running (see AndroidAutoClient's own
                // header comment) -- no need to open the Android Auto
                // screen first anymore for this to work.
                hal::AndroidAutoClient client;
                if (!client.requestConnect()) {
                    std::fprintf(stderr, "custom_ui: auto-start requestConnect() failed (sidecar "
                                 "unreachable)\n");
                } else {
                    // 2026-08-12: per explicit request, the EXISTING
                    // "Auto-start phone projection" setting
                    // (AutoStartCarLink -- settings_screen.cpp,
                    // previously stored but never actually read
                    // anywhere) now controls foreground vs. background:
                    // true = jump straight to the Android Auto screen
                    // (matching real car head units auto-switching to
                    // CarPlay/AA on connect), false = start the
                    // connection silently and leave navigation to the
                    // user manually selecting the AA icon later. Either
                    // way the VIDEO layer itself stays hidden until
                    // that screen is actually the one on display -- see
                    // video_visibility.h -- this flag only controls
                    // whether getting there happens automatically.
                    //
                    // Not done directly here: this runs on its own
                    // background thread, and LVGL/core::navigation must
                    // only ever be touched from the main thread (same
                    // rule core::ReverseGearWatcher documents) -- so
                    // this just records the request; main()'s own loop
                    // polls consume_navigate_request() and does the
                    // actual push() there.
                    bool auto_open = core::default_store().get_bool("AutoStartCarLink", true,
                                                                      "General");
                    if (auto_open) {
                        navigate_pending_.store(true, std::memory_order_release);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Polled from the LVGL main thread (main()'s own loop) once per
    // iteration -- cheap atomic exchange. Returns true at most once per
    // trigger, consuming the request.
    bool consume_navigate_request() {
        return navigate_pending_.exchange(false, std::memory_order_acq_rel);
    }

private:
    std::mutex mtx_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> navigate_pending_{false};
    std::string last_triggered_id_;
    std::string pending_name_;
};

AaAutoStartWatcher & aa_auto_start_watcher() {
    static AaAutoStartWatcher watcher;
    return watcher;
}

}  // namespace

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

        // Wires up the +AAPDEV= -> auto-start-wireless-AA trigger --
        // see AaAutoStartWatcher's own comment above for the full
        // mechanism/evidence. Registering the observer only makes
        // sense once bt.fd is actually open (hal::watch_bluetooth_
        // broadcasts() has nothing to observe otherwise); the reader
        // thread it depends on was already started by shared_handle()
        // itself just above.
        hal::watch_bluetooth_broadcasts(
            [](const std::string & line) { aa_auto_start_watcher().on_broadcast(line); });
        std::thread(&AaAutoStartWatcher::run, &aa_auto_start_watcher()).detach();
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
        // Cheap atomic exchange every iteration -- see
        // AaAutoStartWatcher::run()'s own comment for why this can't
        // just call core::navigation::push() directly from its own
        // background thread. screens.push() (via core::navigation) is
        // itself fine to call repeatedly/from an already-elsewhere
        // navigation state -- same push-based stack every other screen
        // transition in this app already uses.
        if (aa_auto_start_watcher().consume_navigate_request()) {
            core::navigation::push(ui::create_android_auto_screen);
        }
        usleep(5000);
    }

    return 0;
}
