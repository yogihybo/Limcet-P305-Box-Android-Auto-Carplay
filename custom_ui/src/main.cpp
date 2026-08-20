// custom_ui entry point.
//
// Phase 1: HAL (src/hal) owns fbdev/evdev setup, core::ScreenManager
// owns the screen stack, ui:: provides screen factories. main() itself
// is now just wiring + the LVGL tick loop. See docs/IMPLEMENTATION_PLAN.md.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include "lvgl.h"
#include "hal/androidauto_client.h"
#include "hal/bluetooth.h"
#include "hal/display.h"
#include "hal/knob.h"
#include "hal/mcu_input.h"
#include "hal/touch.h"
#include "core/config_store.h"
#include "core/log_timing.h"
#include "core/navigation.h"
#include "core/screen_manager.h"
#include "ui/android_auto_screen.h"
#include "ui/home_screen.h"
#include "ui/theme.h"
#include "ui/staging/home_dashboard.h"
#include "ui/staging/nav_rail.h"

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
    // Debounced per-device, time-windowed: the same phone re-
    // broadcasting +AAPDEV= repeatedly (observed live -- AAPSTAT/AAPDEV
    // can cycle several times over a few seconds around one real
    // detection) only triggers once, not once per broadcast. A
    // DIFFERENT device appearing always triggers -- androidauto::
    // WirelessSessionManager::start() is itself safe to call repeatedly
    // (restarts a fresh attempt, same as a manual "Retry"), so
    // re-triggering isn't unsafe, just wasteful, which this debounce
    // avoids for the common case.
    //
    // 2026-08-13 FIX: this used to be a permanent, whole-process-
    // lifetime debounce (device_id alone, no expiry) -- meaning once a
    // given phone auto-started wireless AA once, it could NEVER
    // auto-start again for the rest of that boot session, even after
    // disconnecting and reconnecting later (drove out of range, BT
    // toggled off, phone rebooted, etc.), since blueware re-broadcasts
    // the identical +AAPDEV= on every fresh detection and device_id
    // never changes for the same phone. No confirmed "+AAPDIS="
    // disconnect broadcast exists to key an explicit reset off of (not
    // observed in any real capture this project has), so this switches
    // to a time-windowed debounce instead: only suppress a repeat for
    // the same device_id within kDebounceWindow of the last trigger,
    // matching the original intent (collapse one detection's own
    // AAPSTAT/AAPDEV cycling) without permanently blocking a real,
    // later reconnection of the same phone.
    void on_broadcast(const std::string & line) {
        constexpr const char * kPrefix = "+AAPDEV=";
        if (line.rfind(kPrefix, 0) != 0) return;
        std::string entry = line.substr(std::strlen(kPrefix));
        std::string mac, name;
        std::string device_id = hal::split_mac_and_name(entry, mac, name) ? mac : entry;

        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mtx_);
        // 2026-08-13: logs every +AAPDEV= this observer actually sees,
        // including debounced-duplicate ones -- per explicit report
        // that auto-start still wasn't happening for a freshly-paired
        // phone even after fixing the boot-order registration race.
        // This makes the next hardware log answer definitively whether
        // the broadcast is even reaching this observer at all (a
        // registration/timing gap, same class as the one already
        // fixed) versus reaching it and being debounced away (a
        // device_id-parsing bug) versus reaching it, triggering
        // requestConnect() correctly, and stalling later inside
        // WirelessSessionManager::run() itself (see BwAapClient::
        // connect()'s new retry loop for the leading real-fix
        // candidate for that last case).
        std::printf("%s ui: +AAPDEV= observed: device_id='%s' name='%s' (last_triggered='%s')\n", core::log_timestamp().c_str(),
                    device_id.c_str(), name.c_str(), last_triggered_id_.c_str());
        constexpr auto kDebounceWindow = std::chrono::seconds(30);
        if (device_id == last_triggered_id_ && (now - last_triggered_at_) < kDebounceWindow) {
            std::printf("%s ui: +AAPDEV= debounced (same device triggered <30s ago)\n", core::log_timestamp().c_str());
            return;
        }
        last_triggered_id_ = device_id;
        last_triggered_at_ = now;
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
                std::printf("%s ui: +AAPDEV= detected ('%s') -- auto-starting wireless "
                            "Android Auto\n", core::log_timestamp().c_str(), name.c_str());

                // 2026-08-13: this device has no RTC and no NTP client
                // anywhere in its rootfs -- the system clock starts at
                // the Unix epoch on every boot and stays there. A
                // detected +AAPDEV= means a real phone is nearby and
                // (per blueware's own AAP_ENABLE feature, see
                // hal/bluetooth.h's top comment) already SDP-confirmed
                // AA-capable, the best available moment to have a real
                // Bluetooth link up to query it -- best-effort, doesn't
                // block anything if it fails (a phone that doesn't
                // answer AT+CCLK over HFP, or isn't actually
                // HFP-connected yet at this exact instant). See
                // hal::sync_clock_from_phone()'s own header comment for
                // why this matters: androidauto/session.cpp's
                // PingRequest.timestamp was leaking "January 1970"
                // straight to the phone during wireless AA sessions, a
                // real suspect for a long-running silent-disconnect
                // bug.
                hal::sync_clock_from_phone(hal::shared_handle());

                // requestConnect() spawns androidauto-sidecar itself if
                // it isn't already running (see AndroidAutoClient's own
                // header comment) -- no need to open the Android Auto
                // screen first anymore for this to work.
                //
                // 2026-08-15: on a device's first-ever auto-trigger the
                // sidecar isn't running yet, so this spawns it fresh --
                // but requestConnect()'s own two attempts happen back
                // to back with no delay between them, both racing the
                // freshly-forked sidecar's own startup (fork+exec+bind)
                // with zero margin. Seen on real hardware: "auto-start
                // requestConnect() failed (sidecar unreachable)" on the
                // very first trigger, permanently dropping that +AAPDEV=
                // (the 30s debounce means no retry until the phone
                // re-broadcasts). ui/android_auto_screen.cpp's own
                // manual "Connect" button calls requestConnect() too,
                // but from the LVGL main thread -- adding a blocking
                // wait inside AndroidAutoClient itself would freeze the
                // UI on every press, not just this cold-start case. This
                // background thread has no such constraint, so the
                // bounded retry lives here instead: one short wait (the
                // sidecar binds its socket "well under one poll
                // interval" per AndroidAutoClient's own header comment,
                // so 300ms is generous) then one more attempt before
                // giving up and logging failure.
                hal::AndroidAutoClient client;
                bool connected = client.requestConnect();
                if (!connected) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    connected = client.requestConnect();
                }
                if (!connected) {
                    std::fprintf(stderr, "%s ui: auto-start requestConnect() failed (sidecar "
                                 "unreachable)\n", core::log_timestamp().c_str());
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
    std::chrono::steady_clock::time_point last_triggered_at_{};
    std::string pending_name_;
};

AaAutoStartWatcher & aa_auto_start_watcher() {
    static AaAutoStartWatcher watcher;
    return watcher;
}

// 2026-08-18: real hardware showed FOUR concurrent custom_ui instances
// (and, since this process spawns its own blueware child --
// hal::ensure_bluetooth_daemon_running() -- four blueware instances
// too) running simultaneously, ~70s after boot, all fighting over the
// same hardware: /dev/fb0, the MCU/knob serial port, blueware's own
// serial port, and (via each spawning its own androidauto-sidecar)
// /dev/fb4 and the ALSA devices. Root cause: this binary was being
// launched manually, repeatedly, during iterative testing with no
// kill-previous-instance step anywhere -- neither in this binary nor
// in the tester's own workflow -- so every fresh test run left the
// prior one still alive. Concurrent, uncoordinated access to shared
// devices from stale instances is a real, plausible confound for
// some of what's been chased elsewhere this session as hardware/
// timing bugs.
//
// flock() on a lock file, not a PID file: a PID file can go stale
// (process died without cleaning it up, e.g. SIGKILL) and then
// falsely block every future launch forever; flock()'s lock is held
// by the kernel against the open file descriptor itself and is
// automatically released the instant this process exits for ANY
// reason, crash included -- no stale-lock cleanup logic needed.
// Non-fatal if the lock file itself can't be created/opened (e.g. a
// read-only /tmp in some future context) -- this is a safety net,
// not something that should block a real, otherwise-working boot.
int acquireSingleInstanceLock() {
    constexpr const char * kLockPath = "/tmp/custom_ui.lock";
    int fd = ::open(kLockPath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "%s ui: open(%s) failed: %s -- continuing without a single-instance guard\n",
                     core::log_timestamp().c_str(), kLockPath, std::strerror(errno));
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "%s ui: another custom_ui instance already holds %s -- refusing to start a "
                     "second one (see this function's own comment for why this matters)\n",
                     core::log_timestamp().c_str(), kLockPath);
        ::close(fd);
        return -2;
    }
    return fd;  // kept open (never closed) for the life of the process -- releasing it early would
                // defeat the whole point.
}

}  // namespace

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // Literal first line -- see core/log_timing.h's own comment. Every
    // log line in this whole process is now on one continuous kernel-
    // dmesg-style timeline.
    core::mark_process_start();

    if (acquireSingleInstanceLock() == -2) {
        return 1;
    }

    std::printf("%s ui: starting, lv_init()...\n", core::log_timestamp().c_str());
    lv_init();
    std::printf("%s ui: lv_init() done\n", core::log_timestamp().c_str());

    lv_display_t * disp = hal::init_display("/dev/fb0");
    if (!disp) {
        std::fprintf(stderr, "%s ui: hal::init_display() failed, exiting\n", core::log_timestamp().c_str());
        return 1;
    }
    std::printf("%s ui: display initialized\n", core::log_timestamp().c_str());

    // Dark/accent theme for every default-styled widget (buttons,
    // switches, sliders, dropdowns, tabview) -- must run before any
    // screen is created, see ui/theme.h.
    ui::theme::init(disp);
    std::printf("%s ui: theme applied\n", core::log_timestamp().c_str());

    // Starts BlueZ 5.66 subsystem (see hal/bluetooth.h) as early as possible.
    hal::ensure_bluetooth_daemon_running();
    std::printf("%s ui: BlueZ daemon launch requested\n", core::log_timestamp().c_str());

    // Auto-spawns androidauto-sidecar if present
    hal::try_spawn_androidauto_sidecar();

    std::thread([]() {
        hal::BluetoothHandle & bt = hal::shared_handle();
        if (bt.fd >= 0) {
            // 2026-08-13: registered BEFORE set_device_name()/
            // auto_reconnect_paired_device() below, not after -- see
            // AaAutoStartWatcher's own comment above for the full
            // +AAPDEV= mechanism. watch_bluetooth_broadcasts() only
            // observes broadcasts live going forward, it never replays
            // anything the reader thread already saw -- and a
            // previously-paired phone's +AAPDEV= (blueware's SDP-
            // capability check) is exactly the kind of broadcast that
            // can fire as a side effect of auto_reconnect_paired_device()'s
            // own connection handshake just below. Registering the
            // observer after that call meant this exact common case --
            // the phone that was already paired before boot -- could
            // have its +AAPDEV= broadcast come and go before anything
            // was listening, silently falling back to requiring the
            // user to open the Android Auto screen and tap Connect
            // manually. The reader thread itself was already running
            // (started by shared_handle() above), so moving only the
            // registration earlier is enough -- no dependency on
            // set_device_name()/auto_reconnect_paired_device() having
            // run first.
            hal::watch_bluetooth_broadcasts(
                [](const std::string & line) { aa_auto_start_watcher().on_broadcast(line); });
            std::thread(&AaAutoStartWatcher::run, &aa_auto_start_watcher()).detach();

            // apply the configured Bluetooth name every boot,
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
                std::printf("%s ui: bluetooth device name set to '%s'\n", core::log_timestamp().c_str(), btName.c_str());
            } else {
                std::fprintf(stderr, "%s ui: failed to set bluetooth device name to '%s'\n", core::log_timestamp().c_str(),
                             btName.c_str());
            }
            hal::auto_reconnect_paired_device(bt);
        }
    }).detach();

    // Process-lifetime, intentionally never freed -- same convention as
    // every other process-lifetime singleton in this codebase. Touch,
    // knob rotation, and knob push-button all share this one
    // /dev/ttyHS0 connection -- see hal/mcu_input.h for why (one reader
    // thread per fd).
    static hal::McuInputHal mcu_input("/dev/ttyHS0");
    bool mcu_ok = mcu_input.start();
    std::printf("%s ui: MCU input (touch/knob) %s\n", core::log_timestamp().c_str(), mcu_ok ? "started" : "unavailable");

    lv_indev_t * touch = mcu_ok ? hal::init_touch(mcu_input) : nullptr;
    std::printf("%s ui: touch %s\n", core::log_timestamp().c_str(), touch ? "initialized" : "unavailable (continuing without it)");

    lv_indev_t * knob = mcu_ok ? hal::init_knob(mcu_input) : nullptr;
    if (knob) {
        lv_indev_set_group(knob, core::navigation::focus_group());
    }
    std::printf("%s ui: knob %s\n", core::log_timestamp().c_str(), knob ? "initialized" : "unavailable (continuing without it)");

    core::ScreenManager screens;
    core::navigation::init(screens);  // lets Settings/Bluetooth screens
                                       // push/pop without a captured
                                       // ScreenManager* -- see core/navigation.h
    // 2026-08-19: swapped in the new Material-3 staging_ui home
    // dashboard (previously ui::create_home_screen, still compiled and
    // available -- android_auto_screen.cpp/bluetooth_screen.cpp/
    // reverse_camera_screen.cpp/status_bar.cpp are all unaffected,
    // every other screen still navigates back via
    // core::navigation::pop(), not by calling this directly).
    // Deliberately did NOT swap ui::theme::init() above for
    // staging_ui::theme::init() -- both call lv_theme_default_init()
    // globally for the whole display, and those four other screens
    // rely on ui::theme's palette/font for their own default-widget
    // styling. staging_ui's own screens set colors/fonts explicitly on
    // every element they create (see staging_ui/theme.cpp's style_*()
    // helpers), so they don't depend on the global default theme
    // either way -- the only visible difference from not switching is
    // the one unstyled lv_switch on the Settings System tab picking up
    // ui::theme's blue accent instead of staging_ui's own (both blue).
    screens.push(staging_ui::create_home_dashboard);
    std::printf("%s ui: home dashboard (staging_ui) pushed\n", core::log_timestamp().c_str());

    std::printf("%s ui: LVGL initialized, running main loop\n", core::log_timestamp().c_str());

    // Per-iteration heartbeat logging (iterations/sec once a second)
    // was removed here -- it was a one-time diagnostic for a real bug
    // (LV_MEM_SIZE too small -> lv_malloc() NULL -> the default assert
    // handler's while(1) spin, which looked like a hang; see
    // lv_conf.h's LV_MEM_SIZE comment), confirmed fixed and hardware-
    // tested. Left logging on afterward, it just drowned out real
    // error output on every boot.
    while (true) {
        uint32_t sleep_ms = lv_timer_handler();
        // Cheap atomic exchange every iteration -- see
        // AaAutoStartWatcher::run()'s own comment for why this can't
        // just call core::navigation::push() directly from its own
        // background thread. screens.push() (via core::navigation) is
        // itself fine to call repeatedly/from an already-elsewhere
        // navigation state -- same push-based stack every other screen
        // transition in this app already uses.
        // 2026-08-19: real gap found via code audit -- consume_navigate_
        // request() is edge-triggered (exchange(false), so it can't fire
        // on every tick), but nothing stopped the underlying trigger
        // (+AAPDEV= detection, see AaAutoStartWatcher's own comment)
        // from firing a SECOND time later in the same session (e.g. a
        // mid-drive Bluetooth reconnect) while the user is already on
        // the AA screen -- push() has no dedup of its own, so that would
        // stack a duplicate AA screen (and a duplicate poll_timer_cb()
        // timer, see android_auto_screen.cpp) on top of the current one.
        // hal::androidauto_screen_active() is already the exact "is the
        // AA screen the active one right now" flag that screen itself
        // maintains (set true in create_android_auto_screen(), false in
        // its own screen_delete_cb()) -- reusing it here instead of
        // adding a new ScreenManager API.
        if (aa_auto_start_watcher().consume_navigate_request() &&
            !hal::androidauto_screen_active().load(std::memory_order_acquire)) {
            staging_ui::navigate_to(staging_ui::NavDestination::AndroidAuto);
        }
        // 2026-08-19: was an unconditional 5ms sleep regardless of
        // what lv_timer_handler() actually needed, waking 200 times/sec
        // even on a fully static screen with nothing scheduled for tens
        // of ms. lv_timer_handler() returns the real ms until its next
        // due timer -- use that instead, clamped to [5, 30]ms so touch/
        // knob input still gets serviced promptly (min) while a truly
        // idle screen isn't burning cycles waking up every 5ms (max).
        usleep(std::min(std::max(sleep_ms, 5u), 30u) * 1000);
    }

    return 0;
}
