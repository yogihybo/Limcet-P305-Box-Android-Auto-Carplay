# touch-test

Live diagnostic tool for the ARK1680 resistive ADC touchscreen driver
(`Limcet Hardware/ark1680_ts.c`) — same purpose as `tools/i2c-scan/` was
for the GT911 bus investigation: a static ARM binary to run at the live
`/ #` root shell, since the rootfs has no `devmem2`/`evtest` equivalent.
See `docs/ARK1680_TS_REVERSE_ENGINEERING.md` for the register map this
tool reads.

## Build

Already built and checked in (static, stripped, armhf). Rebuild with:

```
arm-linux-gnueabihf-gcc -static -O2 -Wall -o touch-test touch-test.c
arm-linux-gnueabihf-strip touch-test
```

## Usage

### `touch-test regs`

Dumps the ADC/TSC block (phys `0xe4500000`) and the shared syscon/pinmux
block (phys `0xe4900000`) directly via `/dev/mem` — **works with or
without the `ark1680_ts` driver loaded**, so it's the first thing to run
to confirm the hardware itself is alive before debugging the driver.
Requires root.

```
/ # touch-test regs
```

Run it a few times while touching the panel — `raw_x`/`raw_y` should
change even with no driver bound, since the ADC free-runs once clocked.
If those two registers never move no matter what's touched, the problem
is upstream of software entirely (pinmux/clock enable bits not set, or
the panel isn't wired/seated) — recheck the `ark1680_setup_tsc()`
register writes in `Limcet Hardware/ark1680_ts.c` against this dump.

### `touch-test events /dev/input/eventN`

Opens the given evdev node and prints every event, like a minimal
`evtest`. Use this once the driver is loaded and probed to confirm real
`ABS_X`/`ABS_Y`/`ABS_PRESSURE`/`BTN_TOUCH`/`SYN_REPORT` events come out
when the panel is touched.

```
/ # cat /proc/bus/input/devices | grep -A5 ark1680-ts   # find eventN
/ # touch-test events /dev/input/event0
```

## Suggested debug flow

1. `touch-test regs` — confirm the ADC block is clocked/enabled and
   `raw_x`/`raw_y` respond to touch, independent of the driver.
2. `dmesg | grep ark1680_ts` — confirm probe succeeded and check the
   register dump it logs (see `Limcet Hardware/ark1680_ts.c`'s built-in
   probe-time logging).
3. `echo 1 > /sys/module/ark1680_ts/parameters/debug` — turn on the
   driver's own per-IRQ tracing (raw samples, filtered coordinate,
   stable/unstable verdict) alongside `dmesg -w`.
4. `touch-test events /dev/input/eventN` — confirm userspace actually
   receives the resulting evdev events.

Record findings in `docs/boot_experiment_log.md`.
