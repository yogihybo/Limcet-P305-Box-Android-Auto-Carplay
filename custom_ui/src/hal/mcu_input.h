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
#include <deque>
#include <mutex>
#include <string>
#include <vector>

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

    // CONFIRMED (2026-08-27 wired, 2026-08-31 user-verified end-to-end
    // on real hardware): headlight-driven night mode. Superseded the
    // original 2026-08-21 CMD 0x02-based placeholder -- the real
    // source is CMD 0x01 payload[0] bit 0x02 (see run()'s CMD 0x01
    // case; also docs/MCU_COMMAND_REFERENCE.md's CMD 0x01 row, now
    // ✅). Wired into a real, working feature: this bit feeds
    // night_mode_, main.cpp's loop calls AndroidAutoClient::
    // sendNightMode() on every change, driving AA's real
    // SENSOR_NIGHT_MODE channel -- user confirmed toggling headlights
    // genuinely triggers AA's night mode on real hardware.
    bool get_night_mode() const;

    // Reverse gear state, hardware-confirmed 2026-09-01 via CMD 0x12's
    // payload[0] (0x01=entering, 0x02=exiting) -- both directions on one
    // command, real edge-triggered pushes, not the old "any 0x12 ==
    // disengaged" mapping. Correlated against a real enter-then-exit test
    // cross-checked with MCU Live Log captures (payload=[01 04 00] on
    // entering, [02 01 00] on exiting). Pending a second real-world retest
    // to fully confirm before treating as settled -- if it flips back,
    // check docs/MCU_COMMAND_REFERENCE.md's "reverse-gear command
    // conflict" section first, it has the full history of wrong guesses.
    // CMD 0x04 (previously mismapped as "engaged") is confirmed parking
    // radar/distance telemetry (transRadarLevel), demoted to a no-op --
    // see mcu_input.cpp's own CMD 0x04 case. main.cpp's dual-redundant
    // reverse detection still treats /dev/carback (a real, independent SoC
    // GPIO IRQ driver) as authoritative over this when it's available.
    bool get_reverse_gear() const;

    // MCU Firmware Version string reported via CMD 0x7F
    std::string get_mcu_version() const;

    // Vehicle battery voltage reported via CMD 0x30
    float get_battery_voltage() const;

    // Synchronize UI setting to MCU via CMD 0xA0
    void sync_setting(uint8_t setting_id, uint8_t value);

    // CMD 0x84 (Audio Route) -- the real OEM-bypass audio+video relay
    // control, disassembled 2026-08-29 (see MCU_FIRMWARE_VERIFIED_FINDINGS.md's
    // "CMD 0x84" section). Real firmware only acts on value 0x00 (relay
    // dispatcher state 0) or 0x03 (state 1) -- sends a real "AT+AUDROUTE=1/2"
    // over the MCU's own USART3 and drives the shared GPIOC13/PC2 relay pair.
    // This is the MORE RELIABLE of the two real paths to that relay: its
    // internal gate defaults open, unlike CMD 0xA0 id=0x11's own gate
    // (never confirmed to actually be true in practice). Which physical
    // routing (OEM vs aftermarket) each value corresponds to is NOT
    // confirmed -- tools/mcu-probe's --audio-route command exists to help
    // settle that empirically.
    void sync_audio_route(uint8_t value);

    // CONFIRMED 2026-08-29 by direct disassembly of the confirmed-active
    // MCUAdapter_BoxP300::syncSettingDataToMcu(int)/getSetItemValueTexts(int)
    // (usr/lib/libMcuCenter.so) -- real "Camera Type" setting, CMD 0xA0
    // id=0x01, value 0=AfterMarket Camera / 1=Factory(OEM) Camera (2 more
    // values, 2/3, exist for a paired "360 camera" system but aren't used
    // here). Supersedes and replaces the earlier CanBus_Raise_Toyota lead,
    // which is confirmed dead code on this hardware (see
    // MCU_FIRMWARE_VERIFIED_FINDINGS.md's "RETRACTION" section) -- this is
    // the real mechanism, traced byte-for-byte through the actual send
    // function, not a name-string coincidence.
    void sync_video_relay(bool oem);

    // 2026-08-31: live diagnostic log -- captures every successfully
    // parsed MCU->SoC frame (any cmd, not just the ones this HAL acts
    // on), formatted as one string per frame, in a bounded ring buffer.
    // Exists so custom_ui's own Settings screen can show a live view of
    // real MCU traffic without needing the stock app's factory-menu
    // MCU Monitor screen or a physical UART tap -- since this HAL is
    // the one already reading /dev/ttyHS0 for the running app, it can
    // just expose what it's already seeing. Thread-safe: returns a
    // snapshot copy, safe to call from the LVGL/UI thread on a timer.
    std::vector<std::string> get_recent_frames() const;

private:
    void run();
    void log_frame(unsigned char cmd, const unsigned char * payload, unsigned char len);

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

    static constexpr size_t kFrameLogCapacity = 200;
    mutable std::mutex frame_log_mutex_;
    std::deque<std::string> frame_log_;
};

// Global helper to send CMD 0xA0 settings sync packet to the Limcet MCU
void send_mcu_setting(uint8_t setting_id, uint8_t value);

// Global helper for CMD 0x84 (Audio Route) -- see McuInputHal::sync_audio_route()
void send_mcu_audio_route(uint8_t value);

// Global helper for the real OEM/Aftermarket video-relay toggle -- see
// McuInputHal::sync_video_relay()
void send_mcu_video_relay(bool oem);

// Global getter for the active MCU instance
McuInputHal * get_mcu_instance();

// Global helper for the live MCU traffic log -- see
// McuInputHal::get_recent_frames(). Returns an empty vector if no MCU
// instance is running (mirrors get_mcu_version()/get_mcu_battery_voltage()'s
// existing null-safety pattern).
std::vector<std::string> get_mcu_recent_frames();

// Global helpers for version string and voltage telemetry
std::string get_mcu_version();
float get_mcu_battery_voltage();

}  // namespace hal
