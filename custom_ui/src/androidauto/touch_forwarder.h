#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <linux/input.h>

#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include "androidauto/input_channel.h"

namespace androidauto {

// ⚠️ REAL HARDWARE FINDING (2026-08-11), design assumption below not
// currently valid on this board: this class reads a SECOND evdev fd on
// /dev/input/event0, on the assumption that's where real touch data
// lives. A direct test (MsnCoreApp fully disabled, ruling out an
// EVIOCGRAB exclusive-open explanation) showed /dev/input/event0
// delivers zero touch events on this hardware -- real touch coordinates
// are relayed by the Limcet MCU over /dev/ttyHS0 instead. See
// docs/MCU_ADAPTERS.md's "MCU role -- key/status events" section for
// the full capture/analysis trail (confirmed protocol: CMD 0x20 frames,
// X=(b4<<8)|b3, Y=(b6<<8)|b5), and src/hal/mcu_touch.h for the userspace
// HAL that already reads this protocol and is wired into
// src/hal/touch.cpp for the local UI's own touch input.
//
// NOT YET updated to match here -- when Phase 2's real aasdk session
// integration lands, this class needs the same fix: read from
// hal::McuTouchHal (or a shared instance of it) instead of opening a
// second /dev/input/event0 fd. Safe to do the same way touch.cpp
// already does: custom_ui and MsnCoreApp are never run concurrently
// (the existing /dev/fb0 handoff model), so exclusively opening
// /dev/ttyHS0 here doesn't conflict with anything -- no kernel changes
// needed, no backwards-compatibility concern. Deferred purely because
// androidauto/'s aasdk session isn't even linked into custom_ui's own
// binary today (a separate standalone test-tool build target, see
// Makefile), so nothing currently exercises this path -- not deferred
// for any technical blocker.
//
// Original design rationale below, still accurate for what this class
// actually does today (reads evdev) -- just not accurate about that
// data being real touch input on this hardware:
//
// Forwards real touch events to Android Auto's InputSourceService, over a
// SECOND, independent evdev fd on the same device node LVGL already reads
// for the UI's own rendering (see src/hal/touch.h). Evdev is a standard
// Linux char device and supports multiple concurrent open()s/readers on
// the same node -- this does NOT dup(), share, or otherwise touch LVGL's
// fd; it opens its own via a plain open() call. If that assumption turns
// out wrong on this kernel (some misc/char drivers reject a second
// open()), open() below will fail and start() just logs and returns
// false -- doesn't take the UI down.
//
// Protocol assumption -- PROTOCOL A, not B, and this is a real finding,
// not a guess: hardware/ark1680_ts.c (this project's own reconstructed
// kernel driver for the ARK1680 resistive touch controller actually on
// this device) only ever calls input_report_abs(ABS_X/ABS_Y/ABS_PRESSURE)
// + input_report_key(BTN_TOUCH) + input_sync() -- no ABS_MT_SLOT,
// ABS_MT_POSITION_X/Y, or ABS_MT_TRACKING_ID anywhere in that file. This
// is a single-touch resistive panel; there is no multitouch protocol B
// stream to parse. pointerId is always 0 here, one active point max.
//
// Coordinate calibration -- also confirmed from that same driver, not
// assumed: input_set_abs_params(ABS_X/Y, 0, 4095, ...) means the kernel
// reports raw 12-bit ADC counts, NOT screen pixels. LVGL's own
// lv_evdev.c does not hardcode this either -- it calls
// ioctl(EVIOCGABS(ABS_X/Y)) at startup to learn the real min/max and
// scales into display coordinates itself (see _evdev_calibrate() there).
// This class does the exact same ioctl query and the exact same linear
// scale into [0, kScreenWidth) x [0, kScreenHeight) (800x480, matching
// what Session::onServiceDiscoveryRequest advertises as this channel's
// touchscreen size) so AA's touch coordinates land in the same space
// LVGL is already putting them in. If EVIOCGABS ever reports a
// degenerate range (min==max, or the ioctl fails), falls back to the
// driver's own hardcoded 0-4095 range rather than dividing by zero.
//
// Async I/O pattern mirrors bluetooth_transport.cpp exactly: a raw fd
// wrapped in boost::asio::posix::stream_descriptor, async_read into a
// fixed-size struct input_event buffer, re-arm from the completion
// handler. struct input_event has a fixed wire size the kernel always
// delivers whole records of (confirmed by lv_evdev.c's own read() loop
// using sizeof(in) directly) -- no partial-record framing to handle.
//
// NOT hardware-tested. Built directly against the driver source and
// LVGL's own evdev implementation, not against a live touch panel.
class TouchForwarder : public std::enable_shared_from_this<TouchForwarder> {
public:
    using Pointer = std::shared_ptr<TouchForwarder>;

    TouchForwarder(boost::asio::io_service &ioService, InputChannel::Pointer inputChannel,
                    std::string eventPath = "/dev/input/event0");
    ~TouchForwarder();

    // Opens the second evdev fd and arms the async read loop. Safe to
    // call only after the phone has actually opened the input channel
    // (see InputChannel's channel-open callback, wired in Session::start)
    // -- sending InputReports before ChannelOpenRequest/Response has
    // completed is not a documented-valid aasdk sequence, and there's no
    // reason to hold this fd open before it's needed anyway. Returns
    // false (and logs) if the device can't be opened or calibrated;
    // non-fatal to the rest of the app either way, matching
    // hal::init_touch's own "missing device isn't fatal" convention.
    bool start();

    // Cancels the pending async read and closes the fd. Safe to call
    // even if start() was never called or already failed.
    void stop();

private:
    void queueRead();
    void handleEvent(const struct input_event &event);
    void sendCalibrated(aap_protobuf::service::inputsource::message::PointerAction action);
    void reportDown();
    void reportMove();
    void reportUp();
    std::uint64_t nowMicros() const;

    boost::asio::io_service &ioService_;
    InputChannel::Pointer inputChannel_;
    std::string eventPath_;
    boost::asio::posix::stream_descriptor device_;

    // Raw ADC calibration range, queried via EVIOCGABS at start() --
    // see header comment. Defaults match the driver's own
    // input_set_abs_params() call in case the ioctl query fails.
    int rawMinX_ = 0;
    int rawMaxX_ = 4095;
    int rawMinY_ = 0;
    int rawMaxY_ = 4095;

    static constexpr std::uint32_t kScreenWidth = 800;
    static constexpr std::uint32_t kScreenHeight = 480;

    // Accumulated across EV_ABS/EV_KEY events until EV_SYN/SYN_REPORT,
    // same accumulate-then-flush-on-SYN pattern lv_evdev.c itself uses
    // (input_event delivers one axis/key change per record, not a whole
    // touch sample at once).
    int pendingRawX_ = 0;
    int pendingRawY_ = 0;
    bool pendingTouchValid_ = false;
    bool touchDown_ = false;
    // True once a DOWN has been sent for the current press -- the first
    // SYN_REPORT while touchDown_ is true sends DOWN, every subsequent
    // one sends MOVED, reset back to false on release (see handleEvent's
    // SYN_REPORT case).
    bool downReported_ = false;

    // read() target buffer for the current async_read.
    struct input_event readBuffer_ {};
};

}  // namespace androidauto
