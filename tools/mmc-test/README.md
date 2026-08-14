# mmc-test

Automated MMC/SD subsystem check for the live `/ #` root shell — POSIX
shell script, same style as `tools/audio-test/`.

**Read-only by design.** This project had a real `mmc0` corruption
incident (2026-07-13, see `docs/PIN_MASTER_LIST.md`'s "Open items") during
unrelated GPIO brute-force probing — root cause was never conclusively
pinned down. This script never writes to any block device, only reads. Do
not add a write/benchmark test here without re-reading that incident
writeup and making a deliberate decision to accept the risk, with the SD
card backed up first.

## Usage

```
/ # mmc-test.sh
```

## What it checks

1. `mmc0` (SD card slot, already **CONFIRMED** working per
   `docs/HARDWARE_AND_SOC_REFERENCE.md`) — card enumerated, type/name if available.
2. A read-only 10MB spot-check (`dd if=/dev/mmcblk0 of=/dev/null`) —
   confirms the card is still readable without I/O errors, no writes.
3. `mmc1` — **investigative, not pass/fail.** Its DTS comment ("SDIO WiFi
   Controller") is known to be wrong now that WiFi is confirmed to be the
   USB RTL8811CU instead (see `docs/HARDWARE_AND_SOC_REFERENCE.md`'s "Open
   items"). This step just reports what, if anything, is actually
   enumerated there — "nothing" is a valid, useful answer, not a failure.
4. Currently mounted mmc filesystems, from `/proc/mounts`.
