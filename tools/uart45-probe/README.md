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
(baud is genuinely unconfirmed for this link). **Try `-b 9600` first**
-- this session confirmed via the real init-code disassembly that
UART4/UART5 on the MCU side are both actually configured for 9600
baud (see `docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s UART4/5 section),
not a guess. Fall back to 19200/38400/115200 if that's silent, same
order `tools/uart-test/` already uses for this port.

```
killall MsnCoreApp    # frees ttyS2, same requirement as tools/uart-test/
./uart45-probe -b 9600     # try this first -- the confirmed MCU-side baud
./uart45-probe -b 19200
./uart45-probe -b 38400
./uart45-probe             # 115200, the tool's default
```

**Real caveat, checked after this tool was first written -- read before
expecting a result.** The one actual live capture of `ttyS2` traffic
this project has (a 2026-07-22 `strace`, not a direct listen) shows
`MsnCoreApp` sending `[0xFA]...[0xAF]`-framed data, a completely
different frame format from the `[0x55]`-sync protocol this tool
tests. A full check of `can_app.bin` found **zero** trace of
`[0xFA]...[0xAF]` framing anywhere in the image -- so if `ttyS2`'s real
peer is genuinely speaking that captured format, this tool's queries
won't get a meaningful response from it, and a silent result doesn't
cleanly rule out "ttyS2 = this MCU's UART4/5" either way. Still worth
running -- cheap, and the two protocols could coexist on the same link
at different times -- but go in with that context, not the original,
more confident framing. Full reasoning in
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s UART4/5 section.

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
