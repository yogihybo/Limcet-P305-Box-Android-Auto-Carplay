// MCU-relayed touch input HAL.
//
// Real hardware finding (2026-08-11, see docs/MCU_ADAPTERS.md's
// "MCU role -- key/status events" section for the full capture/analysis
// trail): this device's local ADC touch controller path
// (hardware/ark1680_ts.c reading the on-SoC ADC/TSC register block
// directly, exposed to userspace as /dev/input/event0) does NOT deliver
// working touch data on this specific board -- confirmed by a direct
// test with MsnCoreApp fully disabled (ruling out an EVIOCGRAB
// exclusive-open explanation): zero evdev events while the touchscreen
// was actively touched. Real touch coordinates are relayed by the
// Limcet MCU (STM32F105RBT6) over /dev/ttyHS0 as CMD 0x20 "status
// query" frames in the same [0x2E][cmd][len][payload...][checksum]
// protocol tools/mcu-handshake already reverse-engineered and
// hardware-confirmed for other MCU commands.
//
// Safe to open /dev/ttyHS0 exclusively from this app: custom_ui and
// MsnCoreApp are never run concurrently (run_on_device.sh stops
// MsnCoreApp before starting this app, the same handoff model already
// used for /dev/fb0) -- no kernel changes involved either, this is a
// plain userspace serial reader, same as tools/mcu-handshake itself.
//
// Confirmed payload layout for CMD 0x20 (5-byte payload, b3..b7):
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
// down/move/up (see below).
//
// Touch release/idle is signaled by an all-zero CMD 0x20 payload
// (b3=b4=b5=b6=b7=0), confirmed directly in the corner-touch capture
// (every touch event in that capture was bounded by one of these on
// each side). Note this makes (0,0) unreachable as a real touch
// coordinate through this path -- matches every other resistive/ADC
// touch scheme already documented in this project (raw 0 reliably
// means "no contact", not "corner touched exactly at the origin").
//
// This HAL reuses the exact termios/checksum/frame-read logic already
// hardware-confirmed working in tools/mcu-handshake/mcu-handshake.c
// (MCUAdapter_BoxP300 protocol, /dev/ttyHS0 at 38400 8N1) -- not
// re-derived. It also sends that tool's same proactive startup
// sequence (hello/mode-app-changed/state-changed frames) on open,
// since real captures of CMD 0x20 traffic were taken with that sequence
// already sent (mcu-handshake's default, --no-hello was not used) --
// whether it's strictly required for the MCU to start reporting touch
// is not confirmed, but sending it is cheap/harmless (legitimate frames
// real firmware sends anyway) and matches the only configuration this
// project has directly observed working.
//
// NOT YET HARDWARE-TESTED as an LVGL input source -- built directly
// against the confirmed protocol capture/analysis above, not against a
// live touch-driven UI session.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace hal {

struct McuTouchState {
    int32_t x = 0;
    int32_t y = 0;
    bool pressed = false;
};

class McuTouchHal {
public:
    explicit McuTouchHal(std::string port = "/dev/ttyHS0");
    ~McuTouchHal();

    McuTouchHal(const McuTouchHal &) = delete;
    McuTouchHal & operator=(const McuTouchHal &) = delete;

    // Opens the port, configures 38400 8N1 (matching
    // MCUAdapter_BoxP300::getPortSettings(), see mcu-handshake.c),
    // sends the proactive startup sequence, and starts the background
    // read thread. Returns false (non-fatal, matches every other
    // optional-hardware HAL in this codebase) if the port can't be
    // opened/configured.
    bool start();

    // Thread-safe snapshot of the latest touch state. Safe to call from
    // the LVGL main loop every tick (lock-free atomic reads).
    McuTouchState get_state() const;

private:
    void run();

    std::string port_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::atomic<int32_t> x_{0};
    std::atomic<int32_t> y_{0};
    std::atomic<bool> pressed_{false};
};

}  // namespace hal
