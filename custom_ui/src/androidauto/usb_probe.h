#pragma once

namespace androidauto {

// Starts aasdk's real USB hotplug watcher (aasdk::usb::USBHub) and runs
// its io_service for the given number of seconds, logging any device
// that passes aasdk's AOAP accessory-mode query chain (the real wired
// Android Auto handshake: query the device, and if it's not already an
// AOAP device, ask it to switch into accessory mode over control
// transfers). Returns false immediately if libusb_init() fails.
//
// This is deliberately scoped to "detect and log a device," not a full
// Android Auto session -- see docs/IMPLEMENTATION_PLAN.md Phase 2. It
// exists to prove the aasdk static-link graph resolves and actually
// runs on real hardware before building the full protocol/service
// layer on top of it.
bool run_usb_probe(int seconds);

}  // namespace androidauto
