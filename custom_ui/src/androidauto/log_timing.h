#pragma once

// Shared "time since session start" stamp for all androidauto/*.cpp
// diagnostic logging. Added 2026-08-12 while chasing a real hardware
// bug where the phone drops the connection a few seconds after
// ServiceDiscoveryResponse (SensorChannel added, still not resolved as
// of that fix) -- the existing log had no way to tell how much real
// wall-clock time elapsed between "service discovery response sent"
// and the eventual channel-error lines, nor whether specific silent
// steps (audio focus response send, ping sends) actually happened
// before the drop. Every androidauto log line should now be prefixed
// with elapsedMs() so a single hardware log gives a precise timeline
// without needing a live debugger.
//
// Process-wide, not per-Session: this project only ever runs one AA
// session at a time (androidauto-sidecar's own single-session design,
// see main.cpp) so a shared stamp is simpler than threading a
// reference through every channel class's constructor.

#include <chrono>

namespace androidauto {

// Call once, right at the start of Session::start() -- resets the
// zero point for elapsedMs() below. Safe to call again on a retried
// session (wireless_session_manager.cpp restarts the whole Session
// object per attempt); each retry gets its own zero point.
void markSessionStart();

// Milliseconds since the last markSessionStart() call, or 0 if never
// called (e.g. a log line firing before any session has started --
// shouldn't happen in practice, but no reason to crash over it).
long elapsedMs();

}  // namespace androidauto
