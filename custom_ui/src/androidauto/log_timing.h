#pragma once

// Shared "time since process start" stamp for all androidauto-sidecar
// diagnostic logging -- every log line in this process, from the
// moment main() starts (WiFi AP bring-up, Bluetooth handshake, TCP
// accept) through the whole aasdk session lifetime (channels, decode),
// on one continuous timeline. Originally added 2026-08-12 scoped to
// "since session start" and only used by androidauto/*.cpp's Session/
// Channel classes; broadened 2026-08-15 per explicit request to cover
// the whole process and every source file in it, after review found a
// real gap -- wireless_session_manager.cpp/bw_aap_client.cpp/
// usb_probe.cpp/wireless_probe.cpp/bluetooth_rfcomm_server.cpp (the
// whole pre-session connection setup) and hantro_h264_decoder.cpp/
// alsa_output.cpp/touch_forwarder.cpp (session-phase code that
// happened to never get the treatment) had no timing at all, and the
// old per-session reset meant even the covered files went dark before
// a session existed. A single process-wide clock, started once, covers
// every phase with no blind spots -- the only real cost is that "how
// long did this retry take" needs subtracting two printed values
// instead of reading a reset-to-zero one, a small price for never
// having an untimed log line.
//
// Format matches the Linux kernel's own dmesg timestamps
// ([%5ld.%06ld], seconds.microseconds since boot) per explicit
// request -- familiar to anyone who's ever read a kernel log, and
// already the convention every OTHER timestamp in a full boot-to-
// session hardware capture (kernel dmesg lines interleaved with this
// process's own stdout) uses, so the two now visually line up instead
// of looking like two different logging systems.

#include <string>

namespace androidauto {

// Call once, as the literal first line of main() -- see
// sidecars/androidauto/main.cpp. Resets the zero point for
// logTimestamp() below.
void markProcessStart();

// "[%5ld.%06ld]" -- seconds.microseconds since the markProcessStart()
// call, matching dmesg's own format exactly (right-aligned seconds
// field, 6-digit zero-padded microseconds). Returns "[    ?.??????]"
// if called before markProcessStart() -- shouldn't happen in practice
// (main() calls it before anything else can log), but no reason to
// crash over it.
std::string logTimestamp();

}  // namespace androidauto
