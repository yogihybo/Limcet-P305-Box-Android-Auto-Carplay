#include "core/reverse_gear_watcher.h"

namespace core {

ReverseGearWatcher::ReverseGearWatcher(hal::CameraHandle & handle) : handle_(handle) {}

ReverseGearWatcher::~ReverseGearWatcher() {
    running_.store(false, std::memory_order_release);
    // wait_reverse_gear_change() is a blocking read() -- there's no
    // clean way to interrupt it from here short of closing the fd,
    // which is the caller's (hal::close_camera's) job, called after
    // this destructor per the declared ownership in the header. If
    // shutdown ordering ever changes, this join() would hang until
    // the kernel's next carback state change; not a concern today
    // since this process doesn't currently have a shutdown path at
    // all (main.cpp runs an infinite loop, see its own comment).
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool ReverseGearWatcher::start() {
    if (handle_.carback_fd < 0) {
        return false;
    }
    running_.store(true, std::memory_order_release);
    thread_ = core::SizedThread(core::kDefaultThreadStackSize, &ReverseGearWatcher::run, this);
    return true;
}

hal::ReverseGearState ReverseGearWatcher::consume_change() {
    dirty_.store(false, std::memory_order_release);
    return latest_.load(std::memory_order_acquire);
}

void ReverseGearWatcher::run() {
    while (running_.load(std::memory_order_acquire)) {
        hal::ReverseGearState state = hal::wait_reverse_gear_change(handle_);
        if (state == hal::ReverseGearState::Unknown) {
            // Real read() error, not just "no change yet" (the driver's
            // read() blocks until a change or an error -- see
            // hal::wait_reverse_gear_change's own comment). Stop
            // spinning on a broken fd rather than busy-looping.
            break;
        }
        latest_.store(state, std::memory_order_release);
        dirty_.store(true, std::memory_order_release);
    }
}

}  // namespace core
