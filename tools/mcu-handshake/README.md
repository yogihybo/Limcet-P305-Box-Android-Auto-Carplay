# Manual MCU Handshake Tool

This utility (`mcu-handshake`) is a compiled C daemon that runs natively on the target device to emulate the SoC-to-MCU connection handshake and periodic status ping-pong.

### Why is this needed?
The touch panel signals on the Limcet P305/P306 head unit are physically gated by a `CBT16211A` touch-bus switch controlled by the companion STM32 MCU. The MCU only closes this switch once it performs a successful connection handshake with the userspace application `MsnCoreApp` over the High-Speed UART port (`/dev/ttyHS0`).

If you are debugging or testing drivers without running the full `MsnCoreApp` stack (e.g. to isolate kernel driver coordinate delivery or during development), running this tool simulates the handshake in the background, signaling the MCU to close the switch and activate the touch panel physical line.

### How it works
1. Opens `/dev/ttyHS0` at `38400` baud by default.
2. Listens for incoming MCU packets starting with the RX signature byte `0x2E`.
3. Handles CMD `0x02` (Handshake):
   - Translates incoming mode and sequence bytes to mapped parameters.
   - Builds and replies with a TX response frame starting with `0xFA` and ending with `0xAF` with a valid XOR checksum.
4. Handles CMD `0x20` (Status query):
   - Echoes back the requested status frame bytes formatted correctly.

### Baud rate: confirmed 38400, not a guess
Earlier versions of this tool (and `docs/MCU_ADAPTERS.md`) defaulted to
`115200` as a guess ("try 115200, then 38400"). This is now settled from
the binary itself: `MCUAdapter_BoxP300::getPortSettings()` in
`libMcuCenter.so` (Prado's real, active MCU adapter class, `McuType=6`)
copies a static `PortSettings` struct straight out of `.rodata` with no
computation involved — bytes `00 96 00 00 08 00 00 00 00 00 00 00 00 00
00 00`, i.e. `0x00009600` = **38400** baud, 8 data bits, no parity, 1
stop bit. That's the real value this specific product uses, not one of
several guesses — kept as this tool's default.

### Usage
Run the binary on the device over SSH or the serial console:

```sh
# Ensure MsnCoreApp is stopped to free up the UART port
killall MsnCoreApp

# Start the handshake daemon (defaults to 38400 baud, the confirmed rate)
mcu-handshake

# Run in verbose mode to print hex dumps of RX/TX packets
mcu-handshake --verbose

# Override the port/baud explicitly if needed
mcu-handshake -p /dev/ttyHS0 -b 38400 -v
```

### Scanning multiple baud rates
If 38400 doesn't produce anything — e.g. a different MCU firmware
revision or product variant — `--scan` cycles through a list of common
candidate rates (38400, 115200, 9600, 19200, 57600, plus 230400/460800/
921600 where the toolchain supports them), listening on each for a
configurable duration (default 5s) and reporting raw byte counts, `0x2E`
sync-byte counts, and fully checksum-valid frame counts per rate. It
responds to any valid frame it finds along the way, same as normal mode,
so a handshake can complete opportunistically mid-scan instead of
needing a manual follow-up run:

```sh
killall MsnCoreApp
mcu-handshake --scan          # 5s per candidate baud
mcu-handshake --scan 10 -v    # 10s per candidate, verbose hex dump of everything received
```
Toggle an input on the vehicle (reverse, ACC, a button) while scanning
to prompt MCU traffic — some frames may only be sent on state changes,
not continuously. A summary table prints at the end naming whichever
baud rate(s) produced valid frames; zero bytes at every rate points at
wiring/power/port-contention rather than a baud mismatch.
