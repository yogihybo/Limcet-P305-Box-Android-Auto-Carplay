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
#
# rcS actually boots start_msn_directfb (the GPU/DirectFB QWS backend,
# not plain linuxfb) -- see that script's own comment: `systemonly`
# makes DirectFB allocate its primary surface from /dev/fb0's own
# system memory, meaning killall here may be tearing down a real
# mmap of the same device we're about to open ourselves. 1s was never
# a measured value, just a guess -- bumped to 5s as a real fix after
# hitting an mmap()-area hang during real-hardware testing that a
# longer wait was suspected (not yet confirmed) to fix. If this still
# isn't enough, poll instead of a fixed sleep (e.g. retry open() until
# it succeeds) rather than guessing a larger constant.
if pidof MsnCoreApp > /dev/null 2>&1; then
    echo "run_on_device.sh: stopping MsnCoreApp to free /dev/fb0..."
    killall MsnCoreApp 2>/dev/null || true
    sleep 5
fi

echo "run_on_device.sh: launching $BIN (Ctrl-C to stop)"
exec "$BIN"
