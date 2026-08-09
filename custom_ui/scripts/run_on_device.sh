#!/bin/sh
# Run on the DEVICE itself (over ssh/telnet), not on the build host.
# Standalone test launcher for custom_ui -- iterate without reflashing:
# copy the binary over, run this, Ctrl-C to stop and get the shell
# back, MsnCoreApp is untouched on the next real boot.
#
# Deliberately not an rcS hook: rcS only takes effect after a reflash,
# which defeats the point of fast iteration. This script is the actual
# "test without reflashing" mechanism for Phase 1 -- see
# docs/IMPLEMENTATION_PLAN.md.
#
# Usage: scp build/custom_ui to the device, then on the device:
#   ./run_on_device.sh [path-to-custom_ui-binary]

set -e

BIN="${1:-./custom_ui}"

if [ ! -x "$BIN" ]; then
    echo "run_on_device.sh: $BIN not found or not executable" >&2
    exit 1
fi

# MsnCoreApp holds /dev/fb0 -- custom_ui can't open it while that's
# running. Stop it (rcS will restart it fresh on the next real boot,
# nothing here is persistent).
if pidof MsnCoreApp > /dev/null 2>&1; then
    echo "run_on_device.sh: stopping MsnCoreApp to free /dev/fb0..."
    killall MsnCoreApp 2>/dev/null || true
    sleep 1
fi

echo "run_on_device.sh: launching $BIN (Ctrl-C to stop)"
exec "$BIN"
