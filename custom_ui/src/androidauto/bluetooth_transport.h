#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <aasdk/Transport/Transport.hpp>

namespace androidauto {

// aasdk::transport::Transport backed by a raw Linux BlueZ RFCOMM
// socket (AF_BLUETOOTH/BTPROTO_RFCOMM) instead of aasdk's own
// USB/TCP transports -- aasdk has no Bluetooth transport of its own
// (confirmed by grepping its source tree), because the classic-BT
// RFCOMM connection is a plain POSIX socket, not something aasdk's
// USB/TCP-specific wrapper classes model. This class exists purely to
// give aasdk's real Messenger/Cryptor/Channel stack a third transport
// to run over, mirroring exactly how TCPTransport wraps ITCPEndpoint
// (see aasdk/src/Transport/TCPTransport.cpp) -- but implemented
// directly against Transport's enqueueReceive/enqueueSend rather than
// introducing a parallel "IBluetoothEndpoint" interface aasdk doesn't
// have, since there's only one real socket type here (no host/client
// abstraction split needed the way TCP has for testability).
//
// Uses boost::asio::posix::stream_descriptor to get async read/write
// on the raw socket fd -- AF_BLUETOOTH sockets are plain POSIX file
// descriptors as far as the kernel's read()/write()/epoll are
// concerned, so this works the same way it would for e.g. wrapping a
// pipe or tty fd.
//
// NOT hardware-tested, and deliberately not yet wired into a full
// Session -- the real-device message sequence for the Bluetooth-
// triggered wireless-AA discovery/credential handoff isn't confirmed
// yet (see docs/IMPLEMENTATION_PLAN.md Phase 2's "Wireless AA"
// section). This class is the well-defined, lower-risk half of that
// work: the raw socket plumbing, independent of what gets sent over
// it once that's settled.
class BluetoothRFCOMMTransport : public aasdk::transport::Transport {
public:
    // Takes ownership of an already-connected/accepted RFCOMM socket
    // fd (AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM). Does not open or
    // accept the connection itself -- see bluetooth_rfcomm_server.h
    // for that.
    BluetoothRFCOMMTransport(boost::asio::io_service &ioService, int socketFd);

    void stop() override;

protected:
    void enqueueReceive(aasdk::common::DataBuffer buffer) override;
    void enqueueSend(SendQueue::iterator queueElement) override;

private:
    boost::asio::posix::stream_descriptor socket_;
};

}  // namespace androidauto
