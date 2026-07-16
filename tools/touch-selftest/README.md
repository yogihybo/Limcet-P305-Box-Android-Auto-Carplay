# touch-selftest

Automated wrapper around this project's existing touch test tooling
(`tools/touch-test/`, plus the rootfs's own `tslib` binaries —
`ts_print_raw` etc., discovered already present in
`Prado firmware reconstructed/mtd6_rootfs/rootfs/usr/bin/` this session) —
runs the debug flow already documented in
`tools/touch-test/README.md`'s "Suggested debug flow" with automated
pass/fail parsing, instead of requiring the operator to run and interpret
each command by hand.

**Requires a real physical touch during the timed windows** — this cannot
simulate one for you. Have a finger ready on the panel before running it.

## Usage

```
/ # touch-selftest.sh
```

Expects `tools/touch-test/touch-test` to exist one directory up from
this script (adjust the path inside if you copy it elsewhere on-device).

## What it checks

1. `dmesg` — driver probed and registered.
2. `ark-ts-test regs`, twice 3 seconds apart, **while you touch the
   panel** — confirms the raw ADC hardware itself responds, independent
   of the driver (same technique the underlying tool's own README
   describes).
3. Enables the driver's `debug` module parameter for later `dmesg -w`
   tracing.
4. `ark-ts-test events` on the real evdev node for a 5-second window,
   **while you touch the panel** — confirms real `ABS_X`/`ABS_Y`/
   `BTN_TOUCH`/`SYN_REPORT` events, not just a clean probe.
5. `ts_print_raw` for a 5-second window, as a second, independent
   (tslib-based rather than this project's own tool) cross-check.

(This busybox build has no `timeout` applet — the timed windows are
implemented as background process + `sleep` + `kill` instead.)

## Important caveat

A driver probing cleanly (`dmesg`) is not evidence of correct operation —
per this project's own standing correction, only steps 2, 4, and 5 (which
require an actual physical touch and check for a real signal change) count
as verification. Record findings in `docs/boot_experiment_log.md` and
update `PIN_MASTER_LIST.md`'s "Resistive touch" row.
