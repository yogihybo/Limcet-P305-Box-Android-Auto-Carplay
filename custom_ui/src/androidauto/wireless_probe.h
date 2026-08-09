#pragma once

#include <cstdint>
#include <string>

namespace androidauto {

// Connects out to host:port (a phone already running the Android Auto
// wireless TCP service) and drives the same Session control-channel
// handshake usb_probe.cpp does over USB, but over aasdk::transport::
// TCPTransport instead.
//
// aasdk::tcp::ITCPWrapper only implements the CLIENT side
// (connect/asyncConnect, no accept/listen) -- confirmed by reading its
// interface, not assumed -- meaning aasdk's own design already expects
// the head unit to connect OUT to the phone, not run a listening
// socket. That matches the real Android Auto Wireless architecture:
// the phone hosts the TCP service, the head unit is the client.
//
// This intentionally does NOT yet include the Bluetooth-triggered
// discovery/credential-handoff step (aasdk::channel::bluetooth::
// BluetoothService + aasdk::channel::wifiprojection::
// WifiProjectionService) that would normally supply host/port
// automatically -- the exact real-world message sequence for that
// handoff needs more research before committing code to it (getting
// it wrong risks a component that looks plausible but isn't actually
// how the phone's AA app behaves). host/port are passed in explicitly
// here so the TCPTransport + Session path can be proven independently
// first -- see docs/IMPLEMENTATION_PLAN.md Phase 2's "Wireless AA"
// section.
//
// Not hardware-tested -- host/port must currently be supplied by some
// other means (e.g. a phone already known to be running an AA
// wireless listener on the local network).
bool run_wireless_probe(const std::string &host, std::uint16_t port, int seconds);

}  // namespace androidauto
