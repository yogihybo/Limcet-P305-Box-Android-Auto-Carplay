# usb-test

Automated USB subsystem **regression** check for the live `/ #` root
shell — POSIX shell script, same style as `tools/audio-test/`.

USB is already **CONFIRMED** working end-to-end on this project
(user-confirmed, see `docs/PIN_MASTER_LIST.md`'s driver source table — the
WiFi dongle enumerates over it). This script exists to quickly re-confirm
that after a kernel/DTS rebuild, rather than re-deriving everything from
scratch each time.

## Usage

```
/ # usb-test.sh
```

## What it checks

1. `lsusb` (present in this project's own busybox build — check with
   `busybox --list` if porting elsewhere) or a `/sys/bus/usb/devices`
   fallback; counts USB hubs against the confirmed-working baseline of 2
   (one per MUSB controller, per `docs/new kernel bootlog new uboot
   v11.txt`).
2. `rtl8811cu` module loaded, `wlan0` interface present.
3. USB power-switch GPIOs 117/126 show as claimed in
   `/sys/kernel/debug/gpio`.

A regression here after a kernel/DTS change means go re-check
`PIN_MASTER_LIST.md`'s USB/MUSB row before assuming it's still fine.
