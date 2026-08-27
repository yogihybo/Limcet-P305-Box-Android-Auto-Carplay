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
```
