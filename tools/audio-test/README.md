# audio-test

Automated audio subsystem check for the live `/ #` root shell — same
purpose/style as `tools/i2c-scan/`, `tools/lcd-test/`, etc., but a POSIX
shell script rather than a compiled binary, since this one only needs
tools already present in the rootfs (`aplay`, `amixer`, `/proc/asound`).

Distinguishes what `docs/PIN_MASTER_LIST.md`'s driver source table already
distinguishes: the I2S **data path** (already independently confirmed
working) from the BD37033 **I2C control path** (volume/mute — not confirmed
as of 2026-07-14, see `docs/DRIVER_TEST_PLAN.md` section 6).

## Usage

```
/ # audio-test.sh                  # mechanical checks only
/ # audio-test.sh /path/to/test.wav   # also plays a WAV file via aplay
```

Copy `tools/i2c-scan/i2c-scan` into the same directory (or adjust the path
inside the script) so step 2 can run — it reuses that tool rather than
duplicating the bus scan.

`test-tone.wav` (2s, 440Hz, 44100Hz/16-bit/stereo, fade-in/out to avoid
clicks) is included in this directory for step 5 — neither firmware ships
a WAV file of its own. Copy it alongside the script:
`audio-test.sh ./test-tone.wav`.

## What it checks

1. `/proc/asound/cards` — a sound card is registered at all.
2. `i2c-scan` against i2c-gpio2 — BD37033 ACKs at `0x40`.
3. `amixer scontrols` — the `PA Volume`/`PA Mute` controls exist (names
   taken directly from `sound/soc/arkmicro/BD37033.c`'s
   `bd37033_controls[]`, not guessed).
4. Sets `PA Volume`, reads it back via `amixer` — proves ALSA's own state
   round-trips, but **does not** prove the I2C write reached the chip.
5. Optional `aplay` playback, if a WAV path is given.

## Important caveat

Per this project's own standing correction (see the `PIN_MASTER_LIST.md`
boot-log-evidence caveat and the `feedback_bootlog_evidence_weak` memory):
**a `[PASS]` here is not proof of correct operation.** Steps 1-4 can all
pass mechanically while the BD37033 chip itself never receives a working
write. Only an **audible** volume change while real audio plays confirms
the control path — this script automates the mechanical steps around that
test, it can't listen for you.
