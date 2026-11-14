#pragma once

// 2026-09-03: real hardware finding -- hal/touch.cpp's and hal/knob.cpp's
// LVGL indev read callbacks (mcu_touch_read_cb/mcu_knob_read_cb) call
// AndroidAutoClient::sendTouch()/sendKey()/sendRotary() DIRECTLY, on the
// LVGL main thread, once per touch sample / knob tick while the AA screen
// is active. Those are synchronous AF_UNIX socket round-trips bounded by
// real ~1s SO_RCVTIMEO/SO_SNDTIMEO (see androidauto_client.cpp) -- same
// class of blocking call as the two reverse-gear-triggered freezes fixed
// in commits a18a9c8/b0ff225, just triggered by continuous input instead
// of a discrete gear transition. A detached-thread-per-call fix (like
// those two commits used) isn't safe here: touch DOWN/MOVE/UP and rotary
// ticks must reach the sidecar in the same order they were generated, and
// unordered concurrent detached threads give no such guarantee.
//
// AsyncWorker is a small, reusable single-thread FIFO job queue: the
// producer (LVGL thread) only ever touches a mutex-guarded deque (cheap,
// never blocks on I/O), a single dedicated worker thread drains it in
// order. Bounded queue depth so a genuinely wedged sidecar can't grow
// this without limit on a 173MB/no-swap device -- drops the OLDEST
// droppable pending job when full (see enqueue()), so a recovering
// sidecar gets the most recent input state rather than working through
// a stale backlog.

#include "core/sized_thread.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace core {

class AsyncWorker {
public:
    explicit AsyncWorker(size_t max_queued = 8) : max_queued_(max_queued) {
        thread_ = SizedThread(kDefaultThreadStackSize, &AsyncWorker::run, this);
    }

    ~AsyncWorker() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    AsyncWorker(const AsyncWorker &) = delete;
    AsyncWorker & operator=(const AsyncWorker &) = delete;

    // droppable=false marks a job that must survive queue overflow (e.g. a
    // touch DOWN/UP edge: dropping one leaves the phone thinking a finger
    // is still on the screen, or never down). When full, the OLDEST
    // droppable job is discarded; only if none are droppable does the
    // oldest job go regardless, so the queue stays bounded either way.
    void enqueue(std::function<void()> job, bool droppable = true) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (queue_.size() >= max_queued_) {
                auto victim = queue_.begin();
                for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                    if (it->droppable) { victim = it; break; }
                }
                queue_.erase(victim);
            }
            queue_.push_back({std::move(job), droppable});
        }
        cv_.notify_one();
    }

private:
    void run() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                job = std::move(queue_.front().fn);
                queue_.pop_front();
            }
            job();
        }
    }

    size_t max_queued_;
    std::mutex mtx_;
    std::condition_variable cv_;
    struct Job {
        std::function<void()> fn;
        bool droppable;
    };
    std::deque<Job> queue_;
    bool stopping_ = false;
    SizedThread thread_;
};

}  // namespace core
