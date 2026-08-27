# bd37033-test

Hardware diagnostic, probing, and testing tool for the **Rohm BD37033FV** 5.1-channel digital sound processor on the Limcet BoxP300 head-unit board.

---

## Features

1. **GPIO 34 Standby Line Control**:
   * Exports `/sys/class/gpio/gpio34`, sets direction to `out`, and allows toggling `0` vs `1` to power up the chip's internal I2C logic.
2. **Multi-Bus I2C Probing**:
   * Scans all `/dev/i2c-*` adapters across both GPIO 34 power states (`0` and `1`) specifically for BD37033 addresses (`0x40` and `0x41`).
3. **Power-On Initialization**:
   * Sends the full 20-byte burst (`0x01`–`0x14`) recovered from stock `bd37033_init()`.
4. **Volume Control**:
   * Commands sub-address `0x20` using the reverse-engineered `setVolume()` descending attenuation curve.
5. **Mute Control**:
   * Toggles sub-address `0x06` bit 7.
6. **Pin-Forced Bit-Bang Probe/Verify (`--pinforce-scan` / `--pinforce-verify`)**:
   * The plain `--scan`/`--sweep` bit-bang paths only drive pin 9 (SDA)
     via `/sys/class/gpio`, which has zero electrical effect if the pad's
     silicon is currently muxed away from GPIO -- and a live register
     read (`docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md`) found pin 9
     parked in LCD function (`r7`) at rest. These two commands write the
     raw pinmux register directly (same technique as `tools/pin-force/`)
     to force pin 9 (and pin 121) into real GPIO mode for the exact
     duration of the I2C attempt, then restore LCD function immediately
     after -- a fair test of the theory that GPIO9(SDA)/GPIO121(SCL) is
     the real bus (independently corroborated by the vendor's own
     `ark1668_tyw_zksw.dts` reference board, not just this project's own
     dts). `--pinforce-scan` does a quick ACK probe at both `0x40`/`0x41`;
     `--pinforce-verify` does the full 3-test multi-byte sequence
     (unmute/volume/20-byte burst) per address.
7. **Deep Hardware-i2c0 Re-test (`--i2c0-deep`)**:
   * Does **not** dismiss the alternative "dedicated hardware `i2c0`
     controller" theory -- gives it a fair, thorough shot rather than
     relying on the quick `--scan` pass alone. Confirms pins 70/71 are
     actually muxed to `i2c0` function first (forcing them if not,
     mirroring the pin-9 fix), asserts GPIO34 with a 200ms settle
     (vs. `--scan`'s 20ms), and retries the exact vendor call shape
     (`open` -> `ioctl(I2C_SLAVE)` -> single-byte write to sub-address
     `0x01`) up to 5 times per address with per-attempt `errno` reporting.

---

## Usage on Hardware

```bash
# 1. Probe all I2C buses across GPIO34 states
/data/bd37033-test --scan

# 2. Check current GPIO34 status
/data/bd37033-test --status

# 3. Drive GPIO34 LOW (enable)
/data/bd37033-test --gpio 0

# 4. Initialize BD37033 with power-on defaults on confirmed bus (e.g. /dev/i2c-1)
/data/bd37033-test --init /dev/i2c-1 0x40

# 5. Set volume (0-32)
/data/bd37033-test --volume /dev/i2c-1 25

# 6. Unmute
/data/bd37033-test --mute /dev/i2c-1 0

# 7. Fair re-test of the GPIO9/121 bit-bang theory, pin genuinely forced to GPIO
/data/bd37033-test --pinforce-scan
/data/bd37033-test --pinforce-verify

# 8. Fair re-test of the hardware i2c0 theory (does not assume --scan settled it)
/data/bd37033-test --i2c0-deep
```

Both `--pinforce-*` and `--i2c0-deep` need root (raw `/dev/mem` register
access) -- same requirement `tools/pin-force/` already has.
