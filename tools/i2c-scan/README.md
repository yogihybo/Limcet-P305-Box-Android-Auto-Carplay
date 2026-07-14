# i2c-scan

Minimal static ARM binary to scan I²C buses from the live `/ #` busybox
shell on the target board. Built because the rootfs has no `i2c-tools`
and the touch-bus question (hardware `&i2c0` vs bit-banged `i2c-gpio-0`)
has been decided twice by inference/disassembly and reversed both times
(commits `7c7ce4c` then `0be21c7`) with no on-device confirmation. This
settles it empirically instead of by another guess-rebuild-reflash cycle.

## Build

Already built and checked in as `i2c-scan` (static, stripped, armhf).
Rebuild with:

```
arm-linux-gnueabihf-gcc -static -O2 -o i2c-scan i2c-scan.c
arm-linux-gnueabihf-strip i2c-scan
```

## Prerequisite

The DTS (`Limcet Hardware/ark1668-limcet-prado.dts`) currently has
`&i2c0` enabled with **no child devices**, alongside the existing
`i2c-gpio-0` (which still owns `gt911@5d` and `dvr_rn6752@2c`). This
means a kernel built from the current DTS will register **both**
`/dev/i2c-0` and `/dev/i2c-1` (exact numbering may vary — check
`ls /sys/class/i2c-dev/*/name` on-device to see which is which).
`CONFIG_I2C_CHARDEV`, `CONFIG_I2C_DESIGNWARE_PLATFORM`, and
`CONFIG_I2C_GPIO` are all already `=y` in `Limcet Hardware/kernel_dot_config`.

## Usage

1. Copy `i2c-scan` onto the SD rootfs (e.g. into `/usr/bin` or `/root`).
2. Boot the board to the `/ #` prompt.
3. Run:
   ```
   ls /sys/class/i2c-dev/*/name    # identify bus numbers
   ./i2c-scan /dev/i2c-0 /dev/i2c-1
   ```
4. A device ACKing at **0x5d** on a given bus is the GT911. Record which
   bus (hardware `i2c0` vs `i2c-gpio-0`) that is.

## Result

Record findings in `docs/boot_experiment_log.md` under "Systematic I2C
bus verification" — do not just fix the DTS from this result without
writing down the evidence, so it doesn't get silently reversed again
the way `0be21c7` was.
