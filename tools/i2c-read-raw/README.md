# I2C Plain Reader (`i2c-read-raw`)

A lightweight, statically compiled ARM tool that does a **plain multi-byte
I2C read** — no register-address write phase, no repeated start. Just
`ioctl(I2C_SLAVE)` then `read(fd, buf, n)`.

## Why this exists

`i2c-dump` (see `../i2c-dump/`) does a write-register-then-read
transaction for every register — the standard way to read a normal
addressable register file. Against the unidentified device(s) at
`0x10`/`0x11` on `/dev/i2c-0` (found via `i2c-scan`), that came back `XX`
for **every single register**, even though `i2c-scan`'s own plain
single-byte read ACKs fine at those addresses.

That combination — ACKs a bare read, NAKs a register-address write —
doesn't necessarily mean "no device here." It's also exactly what a chip
without a normal addressable register file looks like: a current-address
/ SMBus quick-read style device, a sequential-read EEPROM, or a chip that
just always streams a fixed ID/status word. This tool does the plain read
`i2c-scan` already proved works, just for more than 1 byte, to see what
such a device actually sends by default.

## Usage

```bash
chmod +x ./i2c-read-raw
./i2c-read-raw /dev/i2c-0 0x10
./i2c-read-raw /dev/i2c-0 0x11
```

Defaults to 16 bytes; pass a third argument for more (max 256):

```bash
./i2c-read-raw /dev/i2c-0 0x10 64
```

Output is a hex dump plus an ASCII column (non-printable bytes shown as
`.`), in case the device streams an identifiable model/ID string.

## Reading the output

- Real, non-zero, non-`0xff` bytes that look structured (or spell
  something in the ASCII column) — good sign, note them down and search
  online for the byte pattern alongside "I2C" and the head unit model.
- All `0xff` or all `0x00` — the device ACKed the address but isn't
  actually driving real data onto SDA (could still mean "no device," bus
  floating with pull-ups, or a device that needs a specific command
  first).
- `read()` itself fails (this tool prints the `errno`) — the address
  ACKs a `I2C_SLAVE` ioctl claim but genuinely NAKs any read attempt,
  narrowing it down further.
