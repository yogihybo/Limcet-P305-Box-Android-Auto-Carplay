#include "hal/knob.h"

#include <cstdio>

#include "core/log_timing.h"
#include "hal/androidauto_client.h"

namespace hal {

namespace {

// Real AAOS RotaryController keycodes -- see this file's own header
// comment and androidauto/input_channel.h for the full story. Must
// match session.cpp's ServiceDiscoveryResponse keycodes_supported
// list exactly.
constexpr std::uint32_t kKeycodeSystemNavigationUp = 280;
constexpr std::uint32_t kKeycodeSystemNavigationDown = 281;
constexpr std::uint32_t kKeycodeDpadCenter = 23;
constexpr std::uint32_t kKeycodeDpadLeft = 21;
constexpr std::uint32_t kKeycodeDpadRight = 22;

// Own client instance, separate from android_auto_screen.cpp's/
// status_bar.cpp's -- allow_spawn is always false for sendKey() (see
// its own comment), so this never races anyone else's spawn logic; a
// dedicated instance just keeps this file self-contained rather than
// reaching into UI code from hal/.
AndroidAutoClient & androidauto_client() {
    static AndroidAutoClient client;
    return client;
}

// Edge-detects the push button (McuInputHal::get_knob_pressed() is a
// level/state getter, not an event) so a held press sends exactly one
// tap, not a flood of them for as long as the button stays down.
bool & knob_was_pressed() {
    static bool was_pressed = false;
    return was_pressed;
}

// Tracks whether rotation occurred during the current press-hold
// so hold-and-rotate does not fire an accidental center click on release.
bool & rotated_while_held() {
    static bool rotated = false;
    return rotated;
}

void mcu_knob_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));

    int32_t ticks = mcu->consume_knob_ticks();
    bool pressed = mcu->get_knob_pressed();
    bool press_edge = pressed && !knob_was_pressed();
    bool release_edge = !pressed && knob_was_pressed();
    knob_was_pressed() = pressed;

    if (press_edge) {
        rotated_while_held() = false;
    }

    if (androidauto_screen_active().load(std::memory_order_acquire)) {
        if (ticks != 0) {
            std::printf("%s hal::knob: AA active, ticks=%d, held=%d\n", core::log_timestamp().c_str(), ticks, pressed ? 1 : 0);
        }

        if (pressed && ticks != 0) {
            rotated_while_held() = true;
        }

        /* If held while rotating -> card nudge (DPAD_RIGHT / DPAD_LEFT); else intra-card rotary (NAV_DOWN / NAV_UP) */
        std::uint32_t downKey = pressed ? kKeycodeDpadRight : kKeycodeSystemNavigationDown;
        std::uint32_t upKey = pressed ? kKeycodeDpadLeft : kKeycodeSystemNavigationUp;

        for (int32_t i = 0; i < ticks; ++i) {
            androidauto_client().sendKey(downKey);
        }
        for (int32_t i = 0; i < -ticks; ++i) {
            androidauto_client().sendKey(upKey);
        }

        /* Only fire DPAD_CENTER click on release if knob was not rotated while held */
        if (release_edge && !rotated_while_held()) {
            androidauto_client().sendKey(kKeycodeDpadCenter);
        }

        // Report "nothing happened" to LVGL -- this screen has no
        // focusable group to navigate anyway (see this file's header
        // comment), and consume_knob_ticks() has already destructively
        // taken the real value above.
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->enc_diff = static_cast<int16_t>(ticks);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

lv_indev_t * init_knob(McuInputHal & mcu) {
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, mcu_knob_read_cb);
    lv_indev_set_driver_data(indev, &mcu);
    return indev;
}

std::atomic<bool> & androidauto_screen_active() {
    static std::atomic<bool> active{false};
    return active;
}

}  // namespace hal
