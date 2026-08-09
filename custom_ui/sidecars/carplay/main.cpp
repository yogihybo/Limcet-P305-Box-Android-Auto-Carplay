// carplay-sidecar entry point.
//
// Owns all D-Bus/sink complexity in isolation from the main UI binary.
// Connects to sink's existing com.arkmicro.auto service (system bus,
// 4 methods: requestLinkStatus/requestTouchStatus/requestWheelStatus/
// requestKeyValue, plus the onLinkStatusChange signal -- see
// ../../docs/ARCHITECTURE.md), and re-exposes a minimal local protocol
// over a Unix domain socket for the main UI process to consume.
//
// Deliberately NOT D-Bus-to-D-Bus: the main UI binary should not need
// libdbus at all, or any knowledge of sink's raw interface -- just a
// small local socket protocol this sidecar defines. Keeps the UI
// process free to restart/crash independently of sink's own D-Bus
// session state, and vice versa.
//
// Placeholder only -- no libdbus connection, no socket server yet.

#include <cstdio>

int main() {
    std::printf("carplay-sidecar: scaffolding only, nothing implemented yet\n");
    return 0;
}
