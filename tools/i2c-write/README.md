# Raw I2C Register Writer (`i2c-write`)

A lightweight, statically compiled ARM tool that writes a single `[register, value]`
byte pair to an I2C device via `ioctl(I2C_RDWR)`, bypassing any kernel driver bound
to that address.

Built specifically to test the BD37033 sound-processor's persistent
`bd37033_write_byte timeout` failures documented in
`docs/1.5_AUDIO_SUBSYSTEM_INVESTIGATION.md`. The kernel's `BD37033.c` driver has never
successfully written to this chip in this project's testing (nor, per the same
doc, on stock's own kernel) -- but stock's real, working userspace audio-control
code (`libMsnSound.so`/`libMsnCommons.so`) bypasses the kernel driver entirely and
talks to the chip directly via raw ioctls on `/dev/i2c-N`. This tool replicates
that exact access pattern so it can be tested standalone, without needing
`MsnCoreApp` to be running.

Note: both paths ultimately call `i2c_transfer()` on the same underlying adapter,
so this does **not** bypass the electrical bus itself -- if the bus/wiring is
genuinely broken, this will fail identically to the kernel driver. It does bypass
the in-kernel `i2c_client` for that address, which can matter if that client's own
probe-time state or retry policy is part of the problem.

## Usage

```bash
chmod +x ./i2c-write
./i2c-write /dev/i2c-2 0x40 <reg> <val>
```

e.g. to test a basic write against BD37033 on this project's assigned bus:

```bash
./i2c-write /dev/i2c-2 0x40 0x00 0x00
```

Optional 5th argument overrides the retry count (default 5, matching the kernel
driver's own retry loop):

```bash
./i2c-write /dev/i2c-2 0x40 0x00 0x00 1
```

Prints `OK` with the attempt number on success, or `FAIL` with the per-attempt
`errno` on failure -- compare directly against `dmesg`'s
`bd37033_write_byte timeout` to see whether userspace succeeds where the kernel
driver has not.
