#pragma once

#include <cstdint>

namespace androidauto {

// Opens a raw BlueZ RFCOMM listening socket on the given channel,
// blocks until one phone connects, and returns the connected socket
// fd (caller owns it -- wrap in BluetoothRFCOMMTransport, or close()
// it directly). Returns -1 on any failure (socket/bind/listen/accept),
// logging the errno-based reason to stderr.
//
// Deliberately synchronous/blocking (matches the pattern
// aasdk::tcp::ITCPWrapper::connect() already uses for its own
// synchronous variant) -- this is a bootstrap step that happens once,
// not part of the io_service event loop.
//
// KNOWN GAP, not yet addressed: this does not register an SDP service
// record. A real Android phone's Android Auto app almost certainly
// discovers the head unit's RFCOMM channel via SDP service search (a
// specific service UUID), not by trying channels blindly -- without
// that, only a peer that already knows the channel number (e.g. a
// manual `rfcomm connect` from a paired Linux dev machine, for local
// testing) can actually reach this listener. Adding real SDP
// advertisement (BlueZ's SDP daemon, typically over D-Bus with
// bluetoothd) is unimplemented and untested -- see
// docs/IMPLEMENTATION_PLAN.md Phase 2's "Wireless AA" section.
int accept_rfcomm_connection(std::uint8_t channel);

}  // namespace androidauto
