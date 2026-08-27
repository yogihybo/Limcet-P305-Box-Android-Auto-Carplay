# mcu-probe

Active testing tool for the companion STM32 MCU wire protocol on
`/dev/ttyHS0`. Where `tools/mcu-handshake/` answers "does the
handshake work," this tool answers "what does sending an arbitrary
command actually do" -- built for the BD37033 investigation
(`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`), but general-purpose for any
future MCU-protocol probing.

Reuses `mcu-handshake`'s already hardware-confirmed frame format and
checksum verbatim, not re-derived: `[0x2E][cmd][len][payload...][checksum]`,
checksum = one's-complement of a plain byte sum over `cmd+len+payload`.

## Why a separate tool from `mcu-handshake`

`mcu-handshake` replicates `MsnCoreApp`'s passive startup handshake and
listens. This tool actively sends arbitrary, chosen frames on demand --
single commands, or systematic sweeps -- and reports whatever comes
back. Different job, same wire protocol underneath.

## Important: stop `custom_ui` first

`custom_ui`'s own MCU HAL (`src/hal/mcu_input.cpp`) holds
`/dev/ttyHS0` open exclusively during normal operation. Stop it before
running this tool, the same requirement `mcu-handshake`'s README
already documents for stock's `MsnCoreApp`:

```sh
killall custom_ui   # or however it's supervised (see rcS)
```

## Commands

```sh
# Send CMD 0xA0 [settingId, value] once, listen 300ms for any reply
mcu-probe --setting 0x0b 0x00

# Send an arbitrary raw frame once (cmd byte, then any number of payload bytes)
mcu-probe --send 0x81 0x01

# Sweep all 18 real, disassembly-confirmed CMD 0xA0 setting IDs
# (0x00-0x11, see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md) with one
# test value, one at a time, 500ms apart by default
mcu-probe --sweep-settings 0x01 500

# Sweep raw, empty-payload CMD bytes across a range -- unknown-command
# probing, real physical effects possible, requires explicit confirmation
mcu-probe --sweep-cmds 0x00 0x1f --yes-i-am-sure 400

# Just listen for MCU-originated frames
mcu-probe --listen 10
```

`-p <port>` (default `/dev/ttyHS0`) and `-b <baud>` (default `38400`,
the confirmed real rate) work before the command, same as
`mcu-handshake`.

## Using this for the BD37033 investigation specifically

The two real candidates found in `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`:

```sh
# id=0x0b clearing its flag fires a real 3-pin GPIO enable
# (GPIOA Pin 15 + GPIOB Pin 9 + GPIOB Pin 8) -- the strongest candidate
mcu-probe --setting 0x0b 0x00

# id=0x00 drives GPIOB Pin 1 (a real, verified pin -- exact function
# unconfirmed since the earlier "PA_MUTE -> TDA7388" claim was disproven
# by direct physical inspection, no such chip on this board)
mcu-probe --setting 0x00 0x01
```

After either, re-run `bd37033-test --pinforce-verify` (or
`--i2c0-deep`) to check whether BD37033 now ACKs. If it does, that
settles which MCU command is the real enable sequence.

## Safety notes

Real commands in this protocol have confirmed real, physical effects
(buzzer, relays, backlight, display) -- this isn't a purely passive
read. `--sweep-settings` sends known, catalogued commands only.
`--sweep-cmds` probes genuinely unknown command bytes and requires
`--yes-i-am-sure` for that reason -- run it with someone watching/
listening to the vehicle, not unattended, and be ready to Ctrl+C.

## Build

Same convention as `mcu-handshake` -- static ARM binary, no Makefile:

```sh
arm-linux-gnueabihf-gcc -static -Wall -Wextra -O2 -o mcu-probe mcu-probe.c
```
