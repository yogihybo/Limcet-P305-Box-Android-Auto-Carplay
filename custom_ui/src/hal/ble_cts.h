#pragma once

#include <string>

namespace hal {

// 2026-08-20: attempts to read the phone's BLE GATT Current Time
// Service (standard BLE SIG profile, Service UUID 0x1805 / Current Time
// Characteristic 0x2A2B) and set the system clock (CLOCK_REALTIME)
// from it. Tried BEFORE the Bluetooth-PAN/NTP path (hal::bluetooth.cpp's
// sync_clock_from_phone()) -- if it succeeds, the PAN path is skipped
// entirely; if it fails (phone doesn't expose CTS, GATT services never
// resolve, etc.), sync_clock_from_phone() falls through to PAN as
// before. Both paths stay in place deliberately so a real hardware
// comparison can decide which one to actually keep active long-term --
// see this function's own logging, which is intentionally verbose at
// every step for exactly that comparison.
//
// Why this was closed before, and why it's open now: this exact avenue
// was investigated and ruled out on 2026-08-19 (see memory:
// project_clock_sync_avenues_exhausted.md) -- but the reason was
// blueware's OWN firmware, not the hardware: a direct binary byte-
// search found no GATT/CTS table anywhere in blueware's firmware blob
// ("not a dual-mode BT+BLE stack"). The actual chip (RTL8761BTV) is
// genuinely dual-mode Bluetooth 5.0 -- that was blueware never
// exposing BLE/GATT capability, not the silicon lacking it. Now that
// this project runs real bluetoothd instead, GATT client machinery is
// core, always-built code, not gated behind any of this build's
// --disable-* configure flags.
//
// deviceMac: the already-connected paired phone's MAC (see
// hal::bluetooth.cpp's get_connected_device_mac(), the same helper
// sync_clock_from_phone() itself already uses).
//
// Returns true only if a real Current Time value was read and the
// system clock was actually set from it.
bool sync_clock_via_ble_cts(const std::string &deviceMac);

}  // namespace hal
