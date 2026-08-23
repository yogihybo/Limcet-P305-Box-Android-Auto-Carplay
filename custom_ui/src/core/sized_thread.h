#pragma once

// std::thread-like wrapper that spawns with a caller-specified stack size
// instead of glibc's 8MB-per-thread default. 2026-08-23: real hw finding --
// custom_ui's ~67MB VSZ was traced to its own ~7-8 background threads
// (BlueZ D-Bus monitor, AA RFCOMM profile server, bt-agent output
// streamer, MCU touch/knob input, reverse-gear watcher, etc.), each
// reserving 8MB of virtual address space by default even though none of
// them are anywhere near that deep (D-Bus message parsing, popen() output
// capture, simple event loops -- no deep recursion, no large local
// buffers). This device has 173MB total RAM and no swap; keeping every
// thread's *reservation* small is real, cheap headroom on top of the
// (already-fixed) real RSS/page-sharing problem this rootfs migration
// exists to solve -- doesn't change actual physical memory use for
// threads that were never touching much stack anyway, but removes a
// large, unnecessary chunk of virtual bookkeeping.

#include <pthread.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace core {

// Generous for any of this codebase's monitor/event-loop threads (no deep
// recursion, no large stack-resident buffers observed in any of them) --
// 32x smaller than glibc's 8MB default, still far above PTHREAD_STACK_MIN.
constexpr size_t kDefaultThreadStackSize = 256 * 1024;

// For androidauto-sidecar's own heavier threads: the real AASDK session
// loop (Boost.Asio callbacks, Protobuf message dispatch, video/audio
// channel handling) and real-time ALSA audio I/O -- deeper, more
// library-call-heavy stacks than the simple monitor loops above, so a
// larger (but still 8x smaller than glibc's 8MB default) margin.
constexpr size_t kAasdkThreadStackSize = 1024 * 1024;

class SizedThread {
public:
    SizedThread() = default;

    template <typename F, typename... Args>
    explicit SizedThread(size_t stack_size, F && f, Args &&... args) {
        auto bound = std::make_unique<std::function<void()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack_size);

        auto * raw = bound.release();
        int rc = pthread_create(&thread_, &attr, &SizedThread::trampoline, raw);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete raw;
            throw std::runtime_error("core::SizedThread: pthread_create failed");
        }
        joinable_ = true;
    }

    ~SizedThread() {
        // Matches std::thread semantics: a still-joinable thread at
        // destruction time is a real bug (either join() or detach() it),
        // not something to silently paper over.
        if (joinable_) std::terminate();
    }

    SizedThread(const SizedThread &) = delete;
    SizedThread & operator=(const SizedThread &) = delete;

    SizedThread(SizedThread && other) noexcept : thread_(other.thread_), joinable_(other.joinable_) {
        other.joinable_ = false;
    }

    SizedThread & operator=(SizedThread && other) noexcept {
        if (this != &other) {
            if (joinable_) std::terminate();
            thread_ = other.thread_;
            joinable_ = other.joinable_;
            other.joinable_ = false;
        }
        return *this;
    }

    void join() {
        pthread_join(thread_, nullptr);
        joinable_ = false;
    }

    void detach() {
        pthread_detach(thread_);
        joinable_ = false;
    }

    bool joinable() const { return joinable_; }

private:
    static void * trampoline(void * arg) {
        std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()> *>(arg));
        (*fn)();
        return nullptr;
    }

    pthread_t thread_{};
    bool joinable_ = false;
};

}  // namespace core
