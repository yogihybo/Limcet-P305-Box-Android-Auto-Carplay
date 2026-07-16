# I2C Register Dumper (`i2c-dump`)

A lightweight, statically compiled ARM tool designed to dump registers from a specified I2C address using a write-then-read sequence with a repeated start condition.

This is particularly useful for identifying the device IDs or manufacturer registers of unidentified devices on the target system's buses (like the ones at `0x10` and `0x11` on `/dev/i2c-0`).

## Usage

Copy the statically compiled `i2c-dump` binary onto the target system (e.g., via the bootable SD card or using the telnetd prompt) and run:

```bash
chmod +x ./i2c-dump
./i2c-dump /dev/i2c-0 0x10
./i2c-dump /dev/i2c-0 0x11
```

By default, it will dump the first 256 registers (0x00 to 0xFF). If you want to dump a specific number of registers (e.g., only the first 32), specify a third argument:

```bash
./i2c-dump /dev/i2c-0 0x10 32
```

## How to Read the Output

The tool will print a grid of register values:
- A hex value (e.g., `5d`) represents the value read from that register.
- `XX` indicates that the read operation failed (NACK). This is common for registers that do not exist, or if the device is write-only.

## Identification Tips

If registers dump successfully:
1. **Look for constant patterns**: Many chips have hardcoded values (like device ID, vendor ID, revision ID) at register `0x00`, `0x01`, or near the end of the register space (e.g., `0xf0` to `0xff`).
2. **Common register search**: You can search online for the pattern of returned bytes (e.g., `"0x10" I2C register "0x00" value`) along with your head unit model to identify the manufacturer.
