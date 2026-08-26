#include "androidauto/log_timing.h"

#include <chrono>
#include <cstdio>

namespace androidauto {

namespace {
std::chrono::steady_clock::time_point g_processStart;
bool g_started = false;
}  // namespace

void markProcessStart() {
    g_processStart = std::chrono::steady_clock::now();
    g_started = true;
}

std::string logTimestamp() {
    if (!g_started) return "[    ?.??????]";

    auto elapsed = std::chrono::steady_clock::now() - g_processStart;
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    long seconds = static_cast<long>(totalUs / 1000000);
    long micros = static_cast<long>(totalUs % 1000000);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%5ld.%06ld]", seconds, micros);
    return std::string(buf);
}

}  // namespace androidauto
