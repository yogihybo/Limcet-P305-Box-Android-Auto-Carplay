#include "androidauto/bluetooth_transport.h"

// aasdk::error::ErrorCode has no generic "transport failed" or
// Bluetooth-specific entry -- TCP_TRANSFER is the closest existing
// code (a socket-level transfer failure, which is what this is, just
// not literally over TCP). Reused rather than patching aasdk's enum
// for one error code.

namespace androidauto {

BluetoothRFCOMMTransport::BluetoothRFCOMMTransport(boost::asio::io_service &ioService, int socketFd)
    : aasdk::transport::Transport(ioService), socket_(ioService, socketFd) {
}

void BluetoothRFCOMMTransport::stop() {
    boost::system::error_code ec;
    socket_.close(ec);
}

void BluetoothRFCOMMTransport::enqueueReceive(aasdk::common::DataBuffer buffer) {
    boost::asio::async_read(
        socket_, boost::asio::buffer(buffer.data, buffer.size),
        receiveStrand_.wrap([this, self = this->shared_from_this()](
                                 const boost::system::error_code &ec, size_t bytesTransferred) {
            if (!ec) {
                this->receiveHandler(bytesTransferred);
            } else {
                this->rejectReceivePromises(aasdk::error::Error(aasdk::error::ErrorCode::TCP_TRANSFER,
                                                                  ec.value(), ec.message()));
            }
        }));
}

void BluetoothRFCOMMTransport::enqueueSend(SendQueue::iterator queueElement) {
    boost::asio::async_write(
        socket_, boost::asio::buffer(queueElement->first),
        sendStrand_.wrap([this, self = this->shared_from_this(), queueElement](
                              const boost::system::error_code &ec, size_t) {
            if (!ec) {
                queueElement->second->resolve();
            } else {
                queueElement->second->reject(aasdk::error::Error(aasdk::error::ErrorCode::TCP_TRANSFER,
                                                                   ec.value(), ec.message()));
            }

            sendQueue_.erase(queueElement);
            if (!sendQueue_.empty()) {
                this->enqueueSend(sendQueue_.begin());
            }
        }));
}

}  // namespace androidauto
