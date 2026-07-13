# uart-test

Automated passive listen + basic frame check for the live `/ #` root
shell — POSIX shell script, same style as `tools/audio-test/` etc. Covers
`DRIVER_TEST_PLAN.md` sections 2 (MCU link, `/dev/ttyHS0`) and 3 (MSNEry
link, `/dev/ttyS2`).

**Passive only** — this script never writes to either port, it only
reads, following the same "prefer software-only observation" ground rule
as the rest of `DRIVER_TEST_PLAN.md`.

## Usage

```
/ # uart-test.sh
```

## What it checks

1. Kills `MsnCoreApp` (confirmed via `grep -a` on `libMcuCenter.so` to
   hold both `MCUPortName` and `MSNEryPortName` open at runtime — see
   `MCU_ADAPTERS.md`'s 2026-07-14 correction). On this project's own
   reconstructed rootfs, `/etc/profile` no longer auto-respawns it
   (fixed 2026-07-14), so this should stay down for the whole run.
2. `/dev/ttyHS0` — listens at 115200 for 5s, falls back to 38400 if
   silent. If bytes arrive, checks whether the first byte is `0x2E`
   (the `BoxP300` protocol header sig from `MCU_ADAPTERS.md`) to
   distinguish a real frame from noise.
3. `/dev/ttyS2` — tries 115200/9600/19200/38400 in turn (baud
   unconfirmed for this link) until something responds or all four are
   exhausted.

## Implementation notes (things that bit us building this)

- This busybox build has no `timeout` or `stty` applet. Baud is set via
  `microcom -s SPEED` itself (the only baud-setting mechanism available)
  rather than assuming the port's current termios state is correct, and
  timed capture windows use background process + `sleep` + `kill`.
- **`microcom` writes its own diagnostics into the same stream being
  captured** — e.g. `can't tcsetattr for /dev/ttyS2: Input/output error`
  when the port can't be opened/configured at all. A naive byte-count
  check would misreport that error text as "received real data" — this
  was a real false positive caught during local dry-run testing
  (2026-07-14) before ever running this on the device. The script now
  explicitly checks for `microcom`'s known failure phrasing first and
  reports it as an error, not a pass.

## Important caveat

Per this project's boot-log-evidence caveat: this only proves a link is
electrically live and receiving *something* — it does not validate full
protocol correctness. Record findings in `docs/boot_experiment_log.md` and
update `PIN_MASTER_LIST.md`'s hsuart/MSNEry rows.
