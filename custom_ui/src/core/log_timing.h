#pragma once

// Shared "time since process start" stamp for all of custom_ui's own
// diagnostic logging -- every log line in this process, from the
// literal first line of main() (lv_init(), display/bluetooth/MCU-input
// bring-up, screen construction) through the whole running UI, on one
// continuous timeline. Same mechanism and reasoning as androidauto/
// log_timing.h (androidauto-sidecar is a separate process, so needs
// its own independent instance of this, not a shared one) -- added
// 2026-08-15 per explicit request, after review found custom_ui's own
// startup sequence had zero timing at all despite this project's own
// history of real startup-ordering/race bugs (e.g. run_on_device.sh's
// own comment about an mmap()-area hang worked around with a guessed
// delay -- exactly the kind of thing precise timestamps make easier to
// pin down instead of guessing).
//
// Format matches the Linux kernel's own dmesg timestamps
// ([%5ld.%06ld], seconds.microseconds since boot) -- familiar, and
// already what a real hardware capture's interleaved kernel log lines
// use, so both now visually line up.

#include <string>

namespace core {

// Call once, as the literal first line of main().
void mark_process_start();

// "[%5ld.%06ld]" -- seconds.microseconds since the mark_process_start()
// call. Returns "[    ?.??????]" if called before mark_process_start()
// (shouldn't happen in practice).
std::string log_timestamp();

}  // namespace core
