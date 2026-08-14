# audio-test

Automated audio subsystem check for the live `/ #` root shell — same
purpose/style as `tools/i2c-scan/`, `tools/lcd-test/`, etc., but a POSIX
shell script rather than a compiled binary, since this one only needs
tools already present in the rootfs (`aplay`, `amixer`, `/proc/asound`).

Distinguishes what `docs/1.1_HARDWARE_AND_SOC_REFERENCE.md`'s driver source table already
distinguishes: the I2S **data path** (already independently confirmed
working) from the BD37033 **I2C control path** (volume/mute — root-caused to a genuine
hardware/firmware limitation, the chip doesn't respond even at the correct I2C address;
see `docs/1.6_BD37033.md` and `docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md`).

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
5. Static noise, cycled across every real playback device found. Before each
   device's playback, pulses GPIO34 (the BD37033 enable/reset line, see
   `docs/1.6_BD37033.md` section 2) high-then-low rather than trusting whatever
   state `MsnCoreApp` already left it in — a flat re-write of the same value
   is a no-op if the app already drove it there once, so this forces a real
   edge on every run regardless of prior state. Each device gets an explicit
   `sleep 3` listening window after playback, independent of whether `aplay`'s
   own `-d 3` duration flag actually blocks for the full 3 seconds.
6. Optional `aplay` playback of a given WAV file — also pulses GPIO34 first
   and sleeps 3s afterward.

## Important caveat

Per this project's own standing correction (see the `PIN_MASTER_LIST.md`
boot-log-evidence caveat and the `feedback_bootlog_evidence_weak` memory):
**a `[PASS]` here is not proof of correct operation.** Steps 1-4 can all
pass mechanically while the BD37033 chip itself never receives a working
write. Only an **audible** volume change while real audio plays confirms
the control path — this script automates the mechanical steps around that
test, it can't listen for you.
