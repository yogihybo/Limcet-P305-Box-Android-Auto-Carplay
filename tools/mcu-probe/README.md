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

## Why this, not just the SWD test plans, for the relay commands specifically

`docs/MCU_VIDEO_RELAY_SWD_TEST_PLAN.md` and
`docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md` drive the target directly via SWD,
which forces a reset and traps the CPU in the bootloader -- the real
application (and therefore the ArkMicro SoC, held in reset via GPIOB14)
never runs, so those plans can only ever observe the stock/OEM side of the
relay. `--audio-route`/`--video-relay` here instead run entirely from the
Linux side, over the real UART link, against the **real, fully-booted,
already-deployed MCU firmware** actually processing the command through its
real gate logic -- meaning both the stock feed AND the aftermarket
CarPlay/AA display can be watched simultaneously for a real effect. The two
approaches are complementary, not redundant.

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

# CMD 0x84 (Audio Route) -- the real, confirmed OEM-bypass audio+video relay
# control (see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md's "CMD 0x84" section).
# Only values 0x00 and 0x03 have a real, confirmed effect: each sends a real
# "AT+AUDROUTE=1"/"AT+AUDROUTE=2" command over USART3 and drives the shared
# GPIOC13/PC2 relay pair to one of its two states. This is the MORE
# RELIABLE of the two real paths to that relay -- its internal gate defaults
# open, unlike the CMD 0xA0 id=0x11 path below.
mcu-probe --audio-route 0x00   # relay dispatcher state 0
mcu-probe --audio-route 0x03   # relay dispatcher state 1

# CMD 0xA0 id=0x11 -- the OTHER real path to the exact same relay. May have
# NO observable effect: its own gate condition (a struct byte that must
# equal 1) has never been confirmed to actually become true in practice.
# Included for completeness / in case that condition does hold on real
# hardware -- --audio-route above is the better first try.
mcu-probe --video-relay 0x01

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
