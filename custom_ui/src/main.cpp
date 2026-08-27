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
#include "hal/audio.h"
#include "hal/bluetooth.h"
#include "hal/camera.h"
#include "hal/display.h"
#include "hal/display_ctrl.h"
#include "hal/knob.h"
#include "hal/mcu_input.h"
#include "hal/touch.h"
#include "core/config_store.h"
#include "core/log_timing.h"
#include "core/navigation.h"
#include "core/reverse_gear_watcher.h"
#include "core/screen_manager.h"
#include "core/sized_thread.h"
#include "ui/android_auto_screen.h"
#include "ui/home_screen.h"
#include "ui/theme.h"
#include "ui/staging/home_dashboard.h"
#include "ui/staging/nav_rail.h"

namespace {

// 2026-08-20: DEAD CODE as of this project's BlueZ migration -- this
// class's entire trigger (on_broadcast(), below) depends on
// hal::watch_bluetooth_broadcasts()'s observers actually being called,
// and nothing in hal/bluetooth.cpp has called them since blueware's own
// AT-command reader thread (the thing that used to parse +AAPDEV= lines
// off /dev/bw_serial and broadcast them here) was replaced by BlueZ
// D-Bus calls throughout that file. on_broadcast() is registered
// (main() below) but will never fire again. Left in place rather than
// removed -- harmless as unreachable code, and ripping it out wasn't
// worth the risk for this change. The REAL equivalent trigger now is
// hal::aa_profile_server_loop() (hal/bluetooth.cpp) +
// hal::consume_aa_navigate_request(), polled further down in main()'s
// own loop -- same AutoStartCarLink-gated behavior, just off a real
// BlueZ connection event instead of a blueware broadcast that no longer
// happens.
//
// 2026-08-12 (original comment, now describing dead behavior): auto-
// starts the wireless Android Auto session the moment blueware reports
// a nearby phone as Android-Auto-capable, instead of requiring the user
// to manually open the Android Auto screen first.
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
        // WirelessSessionManager::run() itself (see WifiSetupClient::
        // connect()'s new retry loop for the leading real-fix
        // candidate for that last case).
        std::printf("%s [BT] +AAPDEV= observed: device_id='%s' name='%s' (last_triggered='%s')\n", core::log_timestamp().c_str(),
                    device_id.c_str(), name.c_str(), last_triggered_id_.c_str());
        constexpr auto kDebounceWindow = std::chrono::seconds(30);
        if (device_id == last_triggered_id_ && (now - last_triggered_at_) < kDebounceWindow) {
            std::printf("%s [BT] +AAPDEV= debounced (same device triggered <30s ago)\n", core::log_timestamp().c_str());
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
                std::printf("%s [BT] +AAPDEV= detected ('%s') -- auto-starting wireless Android Auto\n",
                            core::log_timestamp().c_str(), name.c_str());

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
                core::SizedThread(core::kDefaultThreadStackSize, []() {
                    hal::sync_clock_from_phone(hal::shared_handle());
                }).detach();

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
                    std::fprintf(stderr, "%s [UI] auto-start requestConnect() failed (sidecar "
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
        std::fprintf(stderr, "%s [UI] open(%s) failed: %s -- continuing without a single-instance guard\n",
                     core::log_timestamp().c_str(), kLockPath, std::strerror(errno));
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "%s [UI] another custom_ui instance already holds %s -- refusing to start a "
                     "second one (see this function's own comment for why this matters)\n",
                     core::log_timestamp().c_str(), kLockPath);
        ::close(fd);
        return -2;
    }
    return fd;  // kept open (never closed) for the life of the process -- releasing it early would
                // defeat the whole point.
}

// 2026-08-27: MCU headlight -> night mode dimming.
// Only dims the physical LCD panel LED backlight intensity via PWM sysfs,
// leaving the VDE color matrix / hue / contrast 100% untouched.
void apply_night_mode_brightness(bool nightMode) {
    int savedBacklight = core::default_store().get_int("Backlight", 100, "General");
    int target = nightMode
                     ? std::max(15, static_cast<int>(savedBacklight * 0.30))
                     : savedBacklight;

    if (hal::set_backlight_brightness(target)) {
        std::printf("%s [HAL:DISP] Night mode %s -- physical backlight set to %d%% (saved=%d%%)\n",
                    core::log_timestamp().c_str(), nightMode ? "ON" : "OFF", target, savedBacklight);
    } else {
        std::fprintf(stderr, "%s [HAL:DISP] apply_night_mode_brightness: set_backlight_brightness failed\n",
                     core::log_timestamp().c_str());
    }
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

    // 2026-08-21: mlockall(MCL_CURRENT|MCL_FUTURE) was added here as a
    // mitigation for the OLD static-linked rootfs's real page-cache-
    // thrashing crash (kswapd0 evicting this binary's own statically-
    // linked code pages under memory pressure, then a real USB-storage
    // disk read to fault them back in -- confirmed via live
    // /proc/meminfo + kworker/usb-storage/kswapd0 activity at the time).
    //
    // 2026-08-24: REMOVED -- real hw evidence (OOM-killer dump,
    // Mem-Info section) shows this is now actively causing the crash
    // class it was meant to prevent, not just no-longer-needed. This
    // session's dynamic-linking migration already fixed the actual
    // root cause (custom_ui's code pages are shared/reclaimable-and-
    // refaultable-from-a-real-shared-library now, not this binary's
    // own private static text). MCL_FUTURE keeps pinning EVERY
    // allocation this process ever makes for the rest of its life, not
    // just what's resident at mlockall() time -- a live OOM dump showed
    // unevictable:78488kB and mlocked:56268kB out of ~169MB total
    // managed memory, well over CMA's own 64MB reservation on top of
    // that, directly starving a completely unrelated process's write()
    // syscall of any reclaimable page-cache headroom. Keeping this
    // active on a device this memory-constrained is strictly worse than
    // the un-mlock'd behavior it was meant to replace.

    std::printf("%s [UI] starting, lv_init()...\n", core::log_timestamp().c_str());
    lv_init();
    std::printf("%s [UI] lv_init() done\n", core::log_timestamp().c_str());

    lv_display_t * disp = hal::init_display("/dev/fb0");
    if (!disp) {
        std::fprintf(stderr, "%s [UI] hal::init_display() failed, exiting\n", core::log_timestamp().c_str());
        return 1;
    }
    std::printf("%s [UI] display initialized\n", core::log_timestamp().c_str());

    // Dark/accent theme for every default-styled widget (buttons,
    // switches, sliders, dropdowns, tabview) -- must run before any
    // screen is created, see ui/theme.h.
    ui::theme::init(disp);
    std::printf("%s [UI] theme applied\n", core::log_timestamp().c_str());

    // Reset hardware VDE color matrix to neutral factory default (no hue/color distortion)
    {
        hal::DisplayCtrlHandle h;
        if (hal::init_display_ctrl(h)) {
            hal::VdeConfig neutral_cfg{0, 128, 128, 128};
            hal::set_vde_config(h, hal::DisplayLayer::Osd1, neutral_cfg);
            hal::close_display_ctrl(h);
        }
    }

    // 2026-08-20: hal/audio.h was #included (commit a43df447, "Unmute
    // DAC/PA on boot") but the actual hal::init_audio_mixer() call was
    // never added anywhere -- the commit only added the include and an
    // unrelated sidecar startup-order change, leaving the DAC/softmaster
    // unmute genuinely dead code. Real hardware symptom this explains:
    // A2DP (bt-agent) and AA media audio (androidauto-sidecar) both
    // decode/write real PCM to ALSA successfully, but nothing is
    // audible on the speakers -- consistent with the ARK-SDDAC hardware
    // DAC channels and/or ALSA softmaster staying at whatever
    // uninitialized/muted default they power on with, since nothing
    // ever ran the amixer unmute commands. amixer settings are ALSA-
    // driver/kernel state, not per-process, so calling this once here
    // (custom_ui, the always-running process) covers androidauto-
    // sidecar and bt-agent's own separate-process ALSA writes too --
    // matches how blueware-era audio worked (MsnCoreApp/start_msn, the
    // stock app, does its own equivalent unmute independently; nothing
    // in custom_ui ever replicated it until now).
    hal::init_audio_mixer();

    core::SizedThread(core::kDefaultThreadStackSize, []() {
        // Starts BlueZ 5.66 subsystem and sidecar in parallel with UI rendering
        hal::ensure_bluetooth_daemon_running();
        std::printf("%s [BT] BlueZ daemon launch requested\n", core::log_timestamp().c_str());
        hal::try_spawn_androidauto_sidecar();

        for (int i = 0; i < 30; ++i) {
            if (hal::is_bluez_active()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

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
            core::SizedThread(core::kDefaultThreadStackSize, &AaAutoStartWatcher::run, &aa_auto_start_watcher()).detach();

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
                std::printf("%s [BT] Bluetooth device name set to '%s'\n", core::log_timestamp().c_str(), btName.c_str());
            } else {
                std::fprintf(stderr, "%s [BT] ERROR: Failed to set bluetooth device name to '%s'\n", core::log_timestamp().c_str(),
                             btName.c_str());
            }

            core::SizedThread(core::kDefaultThreadStackSize, [&bt]() {
                // Settle delay for BlueZ adapter & bt-agent registration
                std::this_thread::sleep_for(std::chrono::seconds(2));

                // Auto-reconnect retry loop: keep attempting to connect to paired phone on boot
                for (int attempt = 1; attempt <= 15; ++attempt) {
                    std::string connectedMac = hal::get_connected_device_mac();
                    if (!connectedMac.empty()) {
                        std::printf("%s [BT] Phone '%s' connected -- auto-reconnect complete\n",
                                    core::log_timestamp().c_str(), connectedMac.c_str());
                        break;
                    }

                    std::printf("%s [BT] Auto-reconnect attempt %d/15...\n",
                                core::log_timestamp().c_str(), attempt);
                    if (hal::auto_reconnect_paired_device(bt)) {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        if (!hal::get_connected_device_mac().empty()) break;
                    } else {
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                    }
                }
            }).detach();
        }
    }).detach();

    static hal::McuInputHal mcu_input("/dev/ttyHS0");
    bool mcu_ok = mcu_input.start();
    std::printf("%s [HAL:MCU] MCU input (touch/knob/buttons) %s\n", core::log_timestamp().c_str(), mcu_ok ? "started" : "unavailable");

    lv_indev_t * touch = mcu_ok ? hal::init_touch(mcu_input) : nullptr;
    std::printf("%s [HAL:MCU] Touchscreen %s\n", core::log_timestamp().c_str(), touch ? "initialized" : "unavailable");

    lv_indev_t * knob = mcu_ok ? hal::init_knob(mcu_input) : nullptr;
    if (knob) {
        lv_indev_set_group(knob, core::navigation::focus_group());
    }
    std::printf("%s [HAL:MCU] Rotary knob %s\n", core::log_timestamp().c_str(), knob ? "initialized" : "unavailable");

    static hal::CameraHandle camera_handle;
    bool camera_ok = hal::init_camera(camera_handle);
    static core::ReverseGearWatcher reverse_watcher(camera_handle);
    if (camera_ok) {
        reverse_watcher.start();
        std::printf("%s [HAL:REVCAM] Reverse gear watcher %s\n", core::log_timestamp().c_str(), camera_ok ? "started" : "unavailable");
    }

    core::ScreenManager screens;
    core::navigation::init(screens);

    screens.push(staging_ui::create_home_dashboard);
    std::printf("%s [UI] Home dashboard active\n", core::log_timestamp().c_str());

    std::printf("%s [UI] Main loop running\n", core::log_timestamp().c_str());

    while (true) {
        uint32_t sleep_ms = lv_timer_handler();

        if (aa_auto_start_watcher().consume_navigate_request() &&
            !hal::androidauto_screen_active().load(std::memory_order_acquire)) {
            staging_ui::navigate_to(staging_ui::NavDestination::AndroidAuto);
        }

        if (hal::consume_aa_navigate_request() &&
            !hal::androidauto_screen_active().load(std::memory_order_acquire)) {
            staging_ui::navigate_to(staging_ui::NavDestination::AndroidAuto);
        }

        // Reverse gear camera auto-trigger (dual-redundant: hardware /dev/carback driver + MCU UART CMD 0x04/0x12)
        static bool lastMcuReverse = false;
        bool mcuReverse = mcu_input.get_reverse_gear();
        bool reverseChanged = false;
        bool reverseEngaged = false;

        if (mcuReverse != lastMcuReverse) {
            reverseChanged = true;
            reverseEngaged = mcuReverse;
            lastMcuReverse = mcuReverse;
        } else if (reverse_watcher.has_pending_change()) {
            hal::ReverseGearState rev = reverse_watcher.consume_change();
            if (rev == hal::ReverseGearState::Engaged) {
                reverseChanged = true;
                reverseEngaged = true;
            } else if (rev == hal::ReverseGearState::Disengaged) {
                reverseChanged = true;
                reverseEngaged = false;
            }
        }

        if (reverseChanged) {
            hal::apply_reversing_volume_cut(reverseEngaged);
            bool factoryCamera = core::default_store().get_bool("OriginalCarCamera", false, "General");
            if (reverseEngaged) {
                if (factoryCamera) {
                    std::printf("%s [HAL:REVCAM] Reverse gear engaged -- OEM Factory Camera mode active (hardware video mux active, SoC overlay bypassed)\n", core::log_timestamp().c_str());
                } else {
                    std::printf("%s [HAL:REVCAM] Reverse gear engaged -- opening aftermarket camera overlay\n", core::log_timestamp().c_str());
                    staging_ui::navigate_to(staging_ui::NavDestination::Camera);
                }
            } else {
                if (factoryCamera) {
                    std::printf("%s [HAL:REVCAM] Reverse gear disengaged -- OEM Factory Camera mode de-activated\n", core::log_timestamp().c_str());
                } else {
                    std::printf("%s [HAL:REVCAM] Reverse gear disengaged -- returning to previous screen\n", core::log_timestamp().c_str());
                    core::navigation::pop();
                }
            }
        }

        {
            static hal::AndroidAutoClient nightModeClient;
            static bool lastNightMode = false;
            static bool nightModeInitialized = false;
            bool nightMode = mcu_input.get_night_mode();
            if (!nightModeInitialized || nightMode != lastNightMode) {
                apply_night_mode_brightness(nightMode);
                nightModeClient.sendNightMode(nightMode);
                lastNightMode = nightMode;
                nightModeInitialized = true;
            }
        }

        {
            static std::chrono::steady_clock::time_point lastMemLog{};
            auto now = std::chrono::steady_clock::now();
            if (lastMemLog.time_since_epoch().count() == 0 ||
                now - lastMemLog >= std::chrono::seconds(60)) {
                lastMemLog = now;
                lv_mem_monitor_t mon{};
                lv_mem_monitor(&mon);
                std::printf("%s [UI] LVGL heap: used=%u%% max_used=%zu bytes free=%zu bytes frag=%u%%\n",
                            core::log_timestamp().c_str(), mon.used_pct,
                            mon.max_used, mon.free_size, mon.frag_pct);
            }
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
