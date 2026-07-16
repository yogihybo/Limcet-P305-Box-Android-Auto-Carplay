# Manual MCU Handshake Tool

This utility (`mcu-handshake`) is a compiled C daemon that runs natively on the target device to emulate the SoC-to-MCU connection handshake and periodic status ping-pong.

### Why is this needed?
The touch panel signals on the Limcet P305/P306 head unit are physically gated by a `CBT16211A` touch-bus switch controlled by the companion STM32 MCU. The MCU only closes this switch once it performs a successful connection handshake with the userspace application `MsnCoreApp` over the High-Speed UART port (`/dev/ttyHS0`).

If you are debugging or testing drivers without running the full `MsnCoreApp` stack (e.g. to isolate kernel driver coordinate delivery or during development), running this tool simulates the handshake in the background, signaling the MCU to close the switch and activate the touch panel physical line.

### How it works
1. Opens `/dev/ttyHS0` at `115200` baud.
2. Listens for incoming MCU packets starting with the RX signature byte `0x2E`.
3. Handles CMD `0x02` (Handshake):
   - Translates incoming mode and sequence bytes to mapped parameters.
   - Builds and replies with a TX response frame starting with `0xFA` and ending with `0xAF` with a valid XOR checksum.
4. Handles CMD `0x20` (Status query):
   - Echoes back the requested status frame bytes formatted correctly.

### Usage
Run the binary on the device over SSH or the serial console:

```sh
# Ensure MsnCoreApp is stopped to free up the UART port
killall MsnCoreApp

# Start the handshake daemon
mcu-handshake

# Run in verbose mode to print hex dumps of RX/TX packets
mcu-handshake --verbose
```
