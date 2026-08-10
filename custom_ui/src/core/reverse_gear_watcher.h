// Reverse-gear listener. Owns a background thread blocked in
// hal::wait_reverse_gear_change() (a real blocking read() on
// /dev/carback) and exposes the latest state as a lock-free flag for
// the LVGL main loop to poll once per tick -- LVGL itself is not
// thread-safe (lv_timer_handler / core::ScreenManager must only be
// touched from the thread running the main loop), so this
// deliberately does NOT call ScreenManager::push/pop from the
// listener thread itself. See main.cpp for the poll-and-act side.
#pragma once

#include <atomic>
#include <thread>

#include "hal/camera.h"

namespace core {

class ReverseGearWatcher {
public:
    // Takes ownership of neither fd in `handle` -- caller (main.cpp)
    // owns hal::CameraHandle's lifetime and must outlive this object.
    explicit ReverseGearWatcher(hal::CameraHandle & handle);
    ~ReverseGearWatcher();

    ReverseGearWatcher(const ReverseGearWatcher &) = delete;
    ReverseGearWatcher & operator=(const ReverseGearWatcher &) = delete;

    // Starts the background thread. No-op (and returns false) if
    // handle.carback_fd was never opened -- matches hal::init_camera's
    // "camera hardware absent is a normal, non-fatal state" pattern.
    bool start();

    // Non-blocking: true if the state has changed since the last
    // consume_change() call. Poll this from the LVGL tick loop.
    bool has_pending_change() const { return dirty_.load(std::memory_order_acquire); }

    // Clears the pending flag and returns the state that triggered it.
    hal::ReverseGearState consume_change();

private:
    void run();

    hal::CameraHandle & handle_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<hal::ReverseGearState> latest_{hal::ReverseGearState::Unknown};
};

}  // namespace core
