#include "androidauto/log_timing.h"

namespace androidauto {

namespace {
std::chrono::steady_clock::time_point g_sessionStart;
bool g_started = false;
}  // namespace

void markSessionStart() {
    g_sessionStart = std::chrono::steady_clock::now();
    g_started = true;
}

long elapsedMs() {
    if (!g_started) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_sessionStart)
        .count();
}

}  // namespace androidauto
