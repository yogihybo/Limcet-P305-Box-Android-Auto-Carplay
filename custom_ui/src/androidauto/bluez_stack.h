#pragma once

namespace androidauto {

// Brings up the real kernel hci0 + BlueZ stack for the AA wireless
// bootstrap, replacing blueware (which /usr/bin/bluez-bringup.sh stops
// first -- the physical UART/chip has exactly one owner at a time,
// blueware and bluetoothd can't run concurrently). start_msn/MsnCoreApp
// is untouched -- it keeps using blueware directly, since it never
// calls anything in this file.
//
// Thin wrapper around firmware_overlay/usr/bin/bluez-bringup.sh (see
// that script's own header) -- reuses the exact hardware-validated
// sequence from tools/rtk-hciattach-test/ and tools/bluetoothd-test/
// rather than reimplementing the GPIO91/H5/D-Bus-daemon dance in C++.
// Blocking, synchronous -- meant to run once on WirelessSessionManager's
// own dedicated thread before BluezClient::connect().
bool bluez_stack_start();

// Stops bluetoothd + rtk_hciattach, freeing the chip back for blueware
// (e.g. once the AA Bluetooth-side handshake is done and the session
// has moved to WiFi -- see wireless_session_manager.cpp).
void bluez_stack_stop();

}  // namespace androidauto
