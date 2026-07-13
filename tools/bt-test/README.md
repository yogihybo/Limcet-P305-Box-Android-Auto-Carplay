# bt-test

Automated Bluetooth link check for the live `/ #` root shell — POSIX shell
script, same style as `tools/audio-test/`.

Targets this unit's **real** BT stack: Feasycom's `blueware` (AT-command
protocol over `/dev/ttyHS1`, see `usr/config.ini`'s opcode table and
`etc/blueware.properties`) — **not** the `rtkbt`/BlueZ-style config that
also ships in this rootfs (`etc/bluetooth/rtkbt.conf`). That file looks
like Android-SDK-template leftover (references `ro.product.model`,
bluedroid-style paths) — the same "shared template, not necessarily used
on this unit" pattern already found for other vestigial config
(`TOUCHSERIAL`/`COMMANDSERIAL`, see `ARK1680_TS_REVERSE_ENGINEERING.md`).
Don't assume `rtkbt` is in play without separately checking.

## Usage

```
/ # bt-test.sh
```

Nothing in `rcS`/`/etc/profile` starts `blueware` automatically on this
rootfs (checked 2026-07-14) — this script starts it as a best-effort test
step if it isn't already running, since no confirmed CLI usage was found
in the binary's strings. If that guess is wrong, start `blueware` manually
however you know works and just re-run the traffic-monitoring portion.

## What it checks

1. GPIO 91 (`BTEN`) — exports and drives it high if not already.
2. `/dev/ttyHS1` exists.
3. `blueware` process — checks if running, starts it otherwise, confirms
   it doesn't immediately exit.
4. 5-second passive read of `/dev/ttyHS1` — any bytes at all confirm the
   link is electrically live (busybox on this build has no `timeout`
   applet, so this is done with background + `sleep` + `kill` instead).

## Important caveat

Per this project's boot-log-evidence caveat: none of these checks prove a
phone actually pairs and streams audio/HID over this link. A real pairing
test with a physical phone is the only thing that fully confirms this.
