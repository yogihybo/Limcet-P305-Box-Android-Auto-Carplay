#pragma once

namespace androidauto {

// Starts aasdk's real USB hotplug watcher (aasdk::usb::USBHub) and runs
// its io_service for the given number of seconds. Any device that
// passes aasdk's AOAP accessory-mode query chain (query the device,
// and if it's not already an AOAP device, ask it to switch into
// accessory mode over control transfers) gets handed to a new
// androidauto::Session, which drives the real control-channel version
// request + SSL handshake (see session.h). Returns false immediately
// if libusb_init() fails.
//
// Scoped to the handshake + service-discovery-request logging, not a
// full session with actual media/input channels open -- see
// docs/IMPLEMENTATION_PLAN.md Phase 2. Pass a generous `seconds`
// window (well beyond a bare detection scan) since it now needs to
// cover the SSL handshake round trips too, not just USB enumeration.
bool run_usb_probe(int seconds);

}  // namespace androidauto
