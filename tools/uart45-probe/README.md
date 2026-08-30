# uart45-probe

Tests the hypothesis that `/dev/ttyS2` ("MSNEry", real traffic
confirmed live but its peer never identified -- see
`docs/1.3_MCU_ADAPTERS.md`, `tools/uart-test/`) is the SoC-side end of
the STM32 companion MCU's own **UART4/UART5** peripherals -- a real,
disassembly-confirmed device-identification handshake protocol found
this session (`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`, "Broad security
sweep" section) that isn't the SoC command link (`USART2`/`ttyHS0`) or
the Bluetooth module (`USART3`, confirmed via its own peripheral base
address) -- its real physical destination was left undetermined.

`docs/1.9_KERNEL_REFERENCE.md`'s own DTS-derived hardware map already
labels the SoC's `&uart3` (`/dev/ttyS2`) as "STM32 companion MCU" -- a
*separate* physical UART link from `ttyHS0` (which uses the SoC's
`ark-hsuart` peripheral, a different controller family). This tool is
the direct test of whether that label is right.

## The real protocol under test

From `can_app.bin`'s own disassembly (not guessed): sync byte `0x55`,
then a type byte selecting one of 5 real sub-messages. Types `0x20` and
`0x32` are the two *outbound* ones -- send `[0x55, 0x20]` or
`[0x55, 0x32]` and the MCU is expected to stream a fixed field back:

| Query | Expected response (from the MCU's real flash-resident data) |
|---|---|
| `0x55 0x20` | `00 00 00 00 FF` (5 bytes -- zeroed placeholder + sentinel) |
| `0x55 0x32` | `"   cD31" 00 93` (9 bytes -- a real, readable identifier string) |

A response matching (or even resembling) either of these confirms the
port under test really is wired to the STM32's UART4/UART5.

## Usage

```
uart45-probe [-p port] [-b baud] [-w window_ms]
```

Defaults: `/dev/ttyS2`, 115200 baud, 1500ms listen window per query
(baud is genuinely unconfirmed for this link -- try 9600/19200/38400
too if 115200 comes back silent, same fallback order
`tools/uart-test/` already uses for this port).

```
killall MsnCoreApp    # frees ttyS2, same requirement as tools/uart-test/
./uart45-probe
./uart45-probe -b 38400
./uart45-probe -b 9600
./uart45-probe -b 19200
```

Also usable against any other candidate port (e.g. `-p /dev/ttyHS0` to
rule out cross-talk, though `ttyHS0`'s own protocol is well-understood
and unrelated to this one).

## Risk

Low -- this only sends 2 bytes of a read-only "identify yourself"
query. No write/erase side effect was found anywhere in the traced
handler for either type byte. Unlike `mcu-probe --reboot-probe`, this
does not touch `CMD 0xE1` or anything that resets/reflashes the chip.
Still stop whatever normally holds the port open first (`MsnCoreApp`
for `ttyS2`), same requirement every other tool in `tools/` documents.

## Build

Same convention as `mcu-probe`/`mcu-handshake` -- static ARM binary, no
Makefile:

```sh
arm-linux-gnueabihf-gcc -static -Wall -Wextra -O2 -o uart45-probe uart45-probe.c
```
