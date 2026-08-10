#include "androidauto/touch_forwarder.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace androidauto {

TouchForwarder::TouchForwarder(boost::asio::io_service &ioService, InputChannel::Pointer inputChannel,
                                std::string eventPath)
    : ioService_(ioService),
      inputChannel_(std::move(inputChannel)),
      eventPath_(std::move(eventPath)),
      device_(ioService_) {
}

TouchForwarder::~TouchForwarder() {
    this->stop();
}

bool TouchForwarder::start() {
    // O_NONBLOCK: boost::asio::posix::stream_descriptor expects a
    // non-blocking fd for its async_read to behave correctly (same
    // requirement bluetooth_transport.cpp's socket-based fd meets
    // implicitly via accept()'s socket flags -- evdev nodes default to
    // blocking mode on open(), so this needs to be explicit here).
    int fd = ::open(eventPath_.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::printf("androidauto: touch forwarder: open(%s) failed: %s\n", eventPath_.c_str(), strerror(errno));
        return false;
    }

    // Calibration query -- see header comment for why this mirrors
    // lv_evdev.c's own EVIOCGABS approach rather than hardcoding 4095.
    struct input_absinfo absInfo {};
    if (::ioctl(fd, EVIOCGABS(ABS_X), &absInfo) == 0 && absInfo.maximum > absInfo.minimum) {
        rawMinX_ = absInfo.minimum;
        rawMaxX_ = absInfo.maximum;
    } else {
        std::printf("androidauto: touch forwarder: EVIOCGABS(ABS_X) unavailable, "
                     "falling back to driver default range [%d,%d]\n", rawMinX_, rawMaxX_);
    }
    if (::ioctl(fd, EVIOCGABS(ABS_Y), &absInfo) == 0 && absInfo.maximum > absInfo.minimum) {
        rawMinY_ = absInfo.minimum;
        rawMaxY_ = absInfo.maximum;
    } else {
        std::printf("androidauto: touch forwarder: EVIOCGABS(ABS_Y) unavailable, "
                     "falling back to driver default range [%d,%d]\n", rawMinY_, rawMaxY_);
    }

    boost::system::error_code ec;
    device_.assign(fd, ec);
    if (ec) {
        std::printf("androidauto: touch forwarder: stream_descriptor::assign failed: %s\n", ec.message().c_str());
        ::close(fd);
        return false;
    }

    std::printf("androidauto: touch forwarder: forwarding %s (raw range x=[%d,%d] y=[%d,%d]) -> %ux%u\n",
                eventPath_.c_str(), rawMinX_, rawMaxX_, rawMinY_, rawMaxY_, kScreenWidth, kScreenHeight);

    this->queueRead();
    return true;
}

void TouchForwarder::stop() {
    boost::system::error_code ec;
    device_.close(ec);
}

void TouchForwarder::queueRead() {
    auto self = this->shared_from_this();
    boost::asio::async_read(
        device_, boost::asio::buffer(&readBuffer_, sizeof(readBuffer_)),
        [this, self](const boost::system::error_code &ec, std::size_t bytesTransferred) {
            if (ec) {
                // operation_aborted is the expected outcome of stop()
                // (device_.close() cancels the pending read) -- not worth
                // logging as an error. Anything else (device unplugged,
                // real I/O error) means we just stop forwarding; not
                // fatal to the rest of the app.
                if (ec != boost::asio::error::operation_aborted) {
                    std::printf("androidauto: touch forwarder: read error: %s\n", ec.message().c_str());
                }
                return;
            }
            if (bytesTransferred == sizeof(readBuffer_)) {
                this->handleEvent(readBuffer_);
            }
            this->queueRead();
        });
}

void TouchForwarder::handleEvent(const struct input_event &event) {
    switch (event.type) {
        case EV_ABS:
            if (event.code == ABS_X) {
                pendingRawX_ = event.value;
                pendingTouchValid_ = true;
            } else if (event.code == ABS_Y) {
                pendingRawY_ = event.value;
                pendingTouchValid_ = true;
            }
            // ABS_PRESSURE is also reported by the driver (0 or 4095) but
            // carries no information BTN_TOUCH doesn't already give us
            // more directly -- not read here.
            break;
        case EV_KEY:
            if (event.code == BTN_TOUCH) {
                bool wasDown = touchDown_;
                touchDown_ = (event.value != 0);
                if (wasDown && !touchDown_) {
                    // UP carries no fresh ABS_X/Y in the driver's own IRQ
                    // handler (see hardware/ark1680_ts.c: the release path
                    // only touches ABS_PRESSURE/BTN_TOUCH) -- report at the
                    // last known-good coordinate rather than waiting on a
                    // SYN_REPORT that may not carry a fresh position.
                    this->reportUp();
                }
            }
            break;
        case EV_SYN:
            if (event.code == SYN_REPORT && touchDown_ && pendingTouchValid_) {
                if (!downReported_) {
                    this->reportDown();
                    downReported_ = true;
                } else {
                    this->reportMove();
                }
            }
            if (event.code == SYN_REPORT && !touchDown_) {
                downReported_ = false;
            }
            pendingTouchValid_ = false;
            break;
        default:
            break;
    }
}

void TouchForwarder::sendCalibrated(aap_protobuf::service::inputsource::message::PointerAction action) {
    if (!inputChannel_) return;

    // See header comment: raw ADC counts -> screen pixels, same linear
    // mapping lv_evdev.c's own _evdev_calibrate() does, so AA's touch
    // coordinates land in the same 800x480 space Session already
    // advertises for this channel.
    std::uint32_t x = static_cast<std::uint32_t>(
        (pendingRawX_ - rawMinX_) * static_cast<long>(kScreenWidth) / (rawMaxX_ - rawMinX_));
    std::uint32_t y = static_cast<std::uint32_t>(
        (pendingRawY_ - rawMinY_) * static_cast<long>(kScreenHeight) / (rawMaxY_ - rawMinY_));

    inputChannel_->sendTouch(x, y, /*pointerId=*/0, action, this->nowMicros());
}

void TouchForwarder::reportDown() {
    this->sendCalibrated(aap_protobuf::service::inputsource::message::ACTION_DOWN);
}

void TouchForwarder::reportMove() {
    this->sendCalibrated(aap_protobuf::service::inputsource::message::ACTION_MOVED);
}

void TouchForwarder::reportUp() {
    this->sendCalibrated(aap_protobuf::service::inputsource::message::ACTION_UP);
}

std::uint64_t TouchForwarder::nowMicros() const {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

}  // namespace androidauto
