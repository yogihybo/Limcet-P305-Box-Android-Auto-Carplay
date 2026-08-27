#include "hal/knob.h"

#include <cstdio>

#include "core/log_timing.h"
#include "hal/androidauto_client.h"

namespace hal {

namespace {

// Android Auto Rotary and Nudge keycodes. Must match
// micro_aap's ServiceDiscoveryResponse keycodes_supported list.
constexpr std::uint32_t kKeycodeNavigatePrevious = 260; // Rotary CCW (Intra-card / App drawer focus prev)
constexpr std::uint32_t kKeycodeNavigateNext = 261;     // Rotary CW (Intra-card / App drawer focus next)
constexpr std::uint32_t kKeycodeDpadLeft = 21;          // Card Nudge Left (Hold + CCW)
constexpr std::uint32_t kKeycodeDpadRight = 22;         // Card Nudge Right (Hold + CW)
constexpr std::uint32_t kKeycodeDpadCenter = 23;        // Select / Click

// Own client instance, separate from android_auto_screen.cpp's/
// status_bar.cpp's -- allow_spawn is always false for sendKey() (see
// its own comment), so this never races anyone else's spawn logic; a
// dedicated instance just keeps this file self-contained rather than
// reaching into UI code from hal/.
AndroidAutoClient & androidauto_client() {
    static AndroidAutoClient client;
    return client;
}

#include <chrono>
#include <cstdint>

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

uint64_t & last_press_time_ms() {
    static uint64_t ms = 0;
    return ms;
}

uint64_t & last_release_time_ms() {
    static uint64_t ms = 0;
    return ms;
}

uint64_t & last_held_rotation_time_ms() {
    static uint64_t ms = 0;
    return ms;
}

void mcu_knob_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    auto * mcu = static_cast<McuInputHal *>(lv_indev_get_driver_data(indev));

    int32_t ticks = mcu->consume_knob_ticks();
    bool raw_pressed = mcu->get_knob_pressed();

    auto now = std::chrono::steady_clock::now();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    bool press_edge = raw_pressed && !knob_was_pressed();
    bool release_edge = !raw_pressed && knob_was_pressed();
    knob_was_pressed() = raw_pressed;

    if (press_edge) {
        last_press_time_ms() = now_ms;
        rotated_while_held() = false;
    }
    if (release_edge) {
        last_release_time_ms() = now_ms;
    }

    if (androidauto_screen_active().load(std::memory_order_acquire)) {
        // Robust Hold-and-Turn detection:
        // A rotation tick is considered "held" if:
        // 1. raw_pressed is actively true, OR
        // 2. press happened within the last 400ms (even if micro-released during detent), OR
        // 3. continuation of an active hold-rotation sequence within 400ms.
        bool is_held = raw_pressed ||
                       (now_ms - last_press_time_ms() < 400 && now_ms - last_release_time_ms() < 350) ||
                       (now_ms - last_held_rotation_time_ms() < 400);

        if (ticks != 0) {
            if (is_held) {
                rotated_while_held() = true;
                last_held_rotation_time_ms() = now_ms;
            }

            std::printf("%s [HAL:KNOB] AA active, ticks=%d, held=%d\n",
                        core::log_timestamp().c_str(), ticks, is_held ? 1 : 0);

            if (is_held) {
                /* Held while rotating -> card nudge (DPAD_RIGHT / DPAD_LEFT) */
                std::uint32_t key = (ticks > 0) ? kKeycodeDpadRight : kKeycodeDpadLeft;
                int32_t count = (ticks > 0) ? ticks : -ticks;
                for (int32_t i = 0; i < count; ++i) {
                    androidauto_client().sendKey(key);
                }
            } else {
                /* Pure native automotive rotary scroll / intra-container traversal */
                androidauto_client().sendRotary(ticks);
            }
        }

        /* Only fire DPAD_CENTER click on release if knob was not rotated while held */
        if (release_edge && !rotated_while_held() && (now_ms - last_held_rotation_time_ms() > 400)) {
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
    data->state = raw_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
