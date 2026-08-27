// MCU-relayed input HAL: touch, rotary knob rotation, and knob/panel
// push-button, all over the same physical /dev/ttyHS0 connection --
// one reader thread here, deliberately, since two threads calling
// read() on the same fd concurrently would corrupt frame boundaries.
//
// Real hardware finding (2026-08-11, see docs/MCU_ADAPTERS.md's
// "MCU role -- key/status events" and "Rotary knob + push-button
// events" sections for the full capture/analysis trail): this
// device's local ADC touch controller path (hardware/ark1680_ts.c
// reading the on-SoC ADC/TSC register block directly, exposed to
// userspace as /dev/input/event0) does NOT deliver working touch data
// on this specific board -- confirmed by a direct test with
// MsnCoreApp fully disabled (ruling out an EVIOCGRAB exclusive-open
// explanation): zero evdev events while the touchscreen was actively
// touched. Real touch coordinates, knob rotation, and knob/panel
// button presses are all relayed by the Limcet MCU (STM32F105RBT6)
// over /dev/ttyHS0 in the same [0x2E][cmd][len][payload...][checksum]
// protocol tools/mcu-handshake already reverse-engineered.
//
// Safe to open /dev/ttyHS0 exclusively from this app: custom_ui and
// MsnCoreApp are never run concurrently (run_on_device.sh stops
// MsnCoreApp before starting this app, the same handoff model already
// used for /dev/fb0) -- no kernel changes involved either, this is a
// plain userspace serial reader, same as tools/mcu-handshake itself.
//
// --- Touch: CMD 0x20 "status query" frames ---
// Confirmed payload layout (5-byte payload, b3..b7):
//   X = (b4 << 8) | b3
//   Y = (b6 << 8) | b5
// Both are direct screen pixel coordinates, no further scaling --
// confirmed by two right-edge corner touches both computing to
// X=800 exactly (this device's real, confirmed screen width) and two
// top/two bottom touches landing near Y=0/Y=480 respectively. b7 (seen
// values 1, 2) is not yet confirmed but its position in the sequence
// (1 on the first sample of a touch, 2 on subsequent samples during a
// hold) is consistent with a touch-down-vs-touch-move state bit --
// treated as informational only here, not required to detect
// down/move/up.
//
// Touch release/idle is signaled by an all-zero CMD 0x20 payload
// (b3=b4=b5=b6=b7=0), confirmed directly in the corner-touch capture
// -- every touch event in that capture was bounded by one of these on
// each side. Note this makes (0,0) unreachable as a real touch
// coordinate through this path.
//
// --- Knob rotation + push button: CMD 0x02 frames ---
// Confirmed payload layout (2-byte payload, b3/b4):
//   b3 = key/rotation code, b4 = 1 (press/active) or 0 (release)
// Live capture, both rotation directions confirmed: b3=64 =
// counter-clockwise, b3=65 = clockwise, both always sent with b4=1 --
// each detent is a momentary "tick" event (no matching b4=0 release,
// unlike a held button), so b4 here means "this tick happened", not
// "currently held". b3=13 alternates cleanly 1 (press) / 0 (release)
// -- a real held button, confirmed the knob's own push-button (not yet
// distinguished from a separate bezel button using the same code, but
// the alternating press/release pattern matches a physical button, not
// a rotation tick). Exposed as a signed per-detent delta:
// consume_knob_ticks() returns +1 per clockwise tick, -1 per
// counter-clockwise tick, since the last call.
//
// This HAL reuses the exact termios/checksum/frame-read logic already
// hardware-confirmed working in tools/mcu-handshake/mcu-handshake.c
// (MCUAdapter_BoxP300 protocol, /dev/ttyHS0 at 38400 8N1) -- not
// re-derived. It also sends that tool's same proactive startup
// sequence (hello/mode-app-changed/state-changed frames) on open,
// since every real capture of MCU traffic this project has was taken
// with that sequence already sent (mcu-handshake's default, --no-hello
// was not used) -- whether it's strictly required is not confirmed,
// but sending it is cheap/harmless (legitimate frames real firmware
// sends anyway) and matches the only configuration this project has
// directly observed working.
//
// Touch is hardware-confirmed working as an LVGL input source (real
// device test, 2026-08-11). Knob/push-button are NOT YET
// hardware-tested as LVGL input sources -- built directly against the
// capture/analysis above.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "core/sized_thread.h"

namespace hal {

struct McuTouchState {
    int32_t x = 0;
    int32_t y = 0;
    bool pressed = false;
};

class McuInputHal {
public:
    explicit McuInputHal(std::string port = "/dev/ttyHS0");
    ~McuInputHal();

    McuInputHal(const McuInputHal &) = delete;
    McuInputHal & operator=(const McuInputHal &) = delete;

    // Opens the port, configures 38400 8N1 (matching
    // MCUAdapter_BoxP300::getPortSettings(), see mcu-handshake.c),
    // sends the proactive startup sequence, and starts the background
    // read thread. Returns false (non-fatal, matches every other
    // optional-hardware HAL in this codebase) if the port can't be
    // opened/configured.
    bool start();

    // Thread-safe snapshot of the latest touch state. Safe to call from
    // the LVGL main loop every tick (lock-free atomic reads).
    McuTouchState get_touch_state() const;

    // Returns the net knob rotation delta (CMD 0x02, b3=65 clockwise
    // = +1/tick, b3=64 counter-clockwise = -1/tick) accumulated since
    // the last call, then resets the counter to 0 -- matches LVGL's
    // own encoder-indev enc_diff contract (report the delta since last
    // read, not a running total).
    int32_t consume_knob_ticks();

    // Thread-safe snapshot of the knob push-button's current state
    // (CMD 0x02, b3=13).
    bool get_knob_pressed() const;

    // 2026-08-21: headlight-driven night mode -- infrastructure wired
    // ahead of the real capture (user confirming the exact code
    // tomorrow), same framing family as the knob (CMD 0x02, b3=sub-
    // code, b4=state) per explicit direction ("same type of framing as
    // the knob input press button but a unique code"). See run()'s own
    // comment for the placeholder sub-code constant this currently
    // matches against -- update ONLY that constant once confirmed, the
    // rest of this chain (display brightness, AA SENSOR_NIGHT_MODE)
    // doesn't need to change.
    bool get_night_mode() const;

    // Reverse gear state directly reported by MCU (CMD 0x04 = engaged, CMD 0x12 = disengaged)
    bool get_reverse_gear() const;

    // MCU Firmware Version string reported via CMD 0x7F
    std::string get_mcu_version() const;

    // Vehicle battery voltage reported via CMD 0x30
    float get_battery_voltage() const;

    // Synchronize UI setting to MCU via CMD 0xA0
    void sync_setting(uint8_t setting_id, uint8_t value);

private:
    void run();

    std::string port_;
    int fd_ = -1;
    core::SizedThread thread_;
    std::atomic<bool> running_{false};

    std::atomic<int32_t> x_{0};
    std::atomic<int32_t> y_{0};
    mutable std::atomic<bool> touch_pressed_{false};

    std::atomic<int32_t> knob_ticks_{0};
    std::atomic<bool> knob_pressed_{false};

    std::atomic<bool> night_mode_{false};
    std::atomic<bool> reverse_gear_{false};
    std::atomic<float> battery_voltage_{0.0f};
    mutable std::atomic<uint64_t> last_touch_ms_{0};

    mutable std::mutex version_mutex_;
    std::string mcu_version_{"Unknown"};
};

// Global helper to send CMD 0xA0 settings sync packet to the Limcet MCU
void send_mcu_setting(uint8_t setting_id, uint8_t value);

}  // namespace hal
