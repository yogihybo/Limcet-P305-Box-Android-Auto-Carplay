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
(`TOUCHSERIAL`/`COMMANDSERIAL`, see `1.8_ARK1680_TS_REVERSE_ENGINEERING.md`).
Don't assume `rtkbt` is in play without separately checking.

## Usage

```
/ # bt-test.sh
```

Nothing in `rcS`/`/etc/profile` starts `blueware` automatically — confirmed
2026-07-17 the real launch site is app code, not init:
`BlueToothAdapter_Blueware::initBlueToothAdapter()` in `libBlueTooth.so`
runs `system("blueware /etc/blueware-bwNNN.properties > /dev/null 2>&1
&")`. This script always kills any already-running `blueware` and
relaunches it itself, captured to `/tmp/blueware.log` instead of that
`/dev/null` redirect — see `docs/1.4_WIRELESS_AND_INIT.md` section 5 ("Who
actually launches `/usr/bin/blueware`") for the full trace of why this
matters: `blueware` has well-instrumented, `errno`-annotated error
messages for every step of the `BTEN` (GPIO 91) export/direction/value
sequence, but `MsnCoreApp`'s own launch throws all of them away.

## What it checks

1. GPIO 91 (`BTEN`) — exports and drives it high if not already.
2. `/dev/ttyHS1` exists.
3. `blueware` process — kills any existing instance, relaunches it fresh
   with output captured (not discarded), confirms it doesn't immediately
   exit, and greps the captured log for `bpio_init ... failed` lines —
   surfacing GPIO 91 failures that are normally invisible.
4. `/dev/bw_serial` and `/dev/bw_iap` exist — the actual app-level
   transport nodes `libBlueTooth.so` talks to (not `/dev/ttyHS1`
   directly), created by `blueware` once it attaches to the module. Also
   checks for the `/dev/socket/goc_rfcom` symlink (set up separately, by
   the app itself, not `blueware`).
5. AT command round-trip on `/dev/bw_serial` — sends `AT+DEVSTAT\r\n`
   (the exact wire format recovered from `libBlueTooth.so`'s
   `writeCommand()`: literal template `"AT+%1\r\n"`) and listens for any
   response within 2 seconds. `DEVSTAT` is a read-only status query, no
   side effects. A response here is the strongest signal short of an
   actual phone pairing that the module is alive and talking.
6. 5-second passive read of `/dev/ttyHS1` — any bytes at all confirm the
   link is electrically live (busybox on this build has no `timeout`
   applet, so this is done with background + `sleep` + `kill` instead).

## Important caveat

Per this project's boot-log-evidence caveat: none of these checks prove a
phone actually pairs and streams audio/HID over this link. A real pairing
test with a physical phone is the only thing that fully confirms this.
