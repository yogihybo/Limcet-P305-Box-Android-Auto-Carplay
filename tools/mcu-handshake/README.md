# Manual MCU Handshake Tool

This utility (`mcu-handshake`) is a compiled C daemon that runs natively on the target device to emulate the SoC-to-MCU connection handshake and periodic status ping-pong.

### Why is this needed?
**Status update (2026-07-26): this CBT16211A/MCU theory is unconfirmed and probably not the real touch-activation gate — see `docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md`'s "Touch activation is gated by `MsnFirstInit`/`/etc/profile`, not the MCU" section for the real, disassembly-confirmed mechanism (a Qt/QWS env var, `QWS_MOUSE_PROTO`/`QWS_ARK_MT_DEVICE`, set by `MsnFirstInit` before `MsnCoreApp` launches).** This theory below was always static-disassembly-only, never hardware-confirmed, and is inconsistent with the directly-observed fact that the AUX+long-press-home mode switch works with `MsnCoreApp` not running at all (`project_limcet_activation_gate` memory) — if a live symptom brought you here expecting this tool to fix touch, try the real mechanism in that doc first.

The touch panel signals on the Limcet P305/P306 head unit are *believed* to be physically gated by a `CBT16211A` touch-bus switch controlled by the companion STM32 MCU. The theory was that the MCU only closes this switch once it performs a successful connection handshake with the userspace application `MsnCoreApp` over the High-Speed UART port (`/dev/ttyHS0`).

If you are debugging or testing drivers without running the full `MsnCoreApp` stack (e.g. to isolate kernel driver coordinate delivery or during development), running this tool simulates the handshake in the background, in case it does still turn out to matter — but don't expect it to be sufficient on its own; it does not replicate `MsnFirstInit`'s Qt env var setup, which is the confirmed prerequisite.

### How it works (corrected 2026-07-18 against real `MCUAdapter_BoxP300` disassembly)
1. Opens `/dev/ttyHS0` at `38400` baud by default.
2. **Sends a startup sequence of three proactive frames**, all confirmed
   by disassembly (not guessed), since it's not yet known which one (if
   any beyond the first) actually triggers the MCU to close the touch
   switch — sending all three is cheap and harmless, they're legitimate
   frames real firmware sends anyway:
   - `cmd=0x81, payload=[0x01]` (`2E 81 01 01 7C`) — `MCUAdapter_BoxP300::onInited()`,
     sent unconditionally at startup before ever waiting to receive anything.
   - `cmd=0x82, payload=[0x01,0x08,0,0,0,0,0,0,0]` (9 bytes) —
     `onModeAppChanged(mode=4)`, the only mode reachable from inside
     `libMcuCenter.so`, fired via `msnAppStateChange`'s bit24/25 path.
   - `cmd=0x84, payload=[0x00,0x03]` — `msnAppStateChange`'s bit26/27
     "state changed" path.
3. Listens for incoming MCU frames (`[0x2E][cmd][len][payload...][checksum]`)
   and logs them (`CMD 0x02` = handshake request, `CMD 0x20` = status
   query, anything else logged generically) — **does not send a wire
   reply to CMD 0x02/0x20**. An earlier version of this tool answered
   those with a `0xFA...0xAF`-framed response; that framing is real
   (`makeProtocolPackage()` in `libMsnCommons.so`) but is `MsnCoreApp`'s
   *internal* IPC format between its own subsystems, never written to
   `/dev/ttyHS0` in the real firmware — replying with it was reaching
   nowhere. The real wire checksum is also different from what that
   version used: a one's-complement of a plain byte sum over
   `cmd+len+payload` (confirmed from `MCUAdapter_BoxP300::getPackageCheckSum()`),
   not XOR.
4. `--no-hello` skips step 2 entirely if you want purely passive listening.

### Baud rate: confirmed 38400, not a guess
Earlier versions of this tool (and `docs/1.3_MCU_ADAPTERS.md`) defaulted to
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
921600 where the toolchain supports them), sending the full startup
sequence and listening on each for a configurable duration (default
5s), reporting the count of valid (checksum-passing) frames received
per rate:

```sh
killall MsnCoreApp
mcu-handshake --scan          # 5s per candidate baud
mcu-handshake --scan 10 -v    # 10s per candidate, verbose hex dump of everything sent/received
```
Toggle an input on the vehicle (reverse, ACC, a button) while scanning
to prompt MCU traffic — some frames may only be sent on state changes,
not continuously. A summary table prints at the end naming whichever
baud rate(s) produced valid frames. Since the startup sequence is sent
at every candidate rate regardless of whether anything comes back, it's
also worth just checking the touch panel directly during/after a scan —
if the switch closes even with zero frames received back, these three
frames alone may be sufficient and reply frames were never the point.

### `/dev/ttyS2` — a second, separate serial channel (2026-07-22)

Found via `strace` while debugging an unrelated display bug (see
`docs/1.3_MCU_ADAPTERS.md`'s "`/dev/ttyS2`" section) — `MsnCoreApp` also opens
`/dev/ttyS2` at **4800 baud** and writes real frames using the
`0xFA...0xAF` format this tool used to build before being corrected to
the `[0x2E]`-framed protocol above. That correction still holds for
`ttyHS0` (zero traffic captured on it) — but the `0xFA...0xAF` framing
genuinely is written to the wire, just on `ttyS2` instead. Frame
structure: `[0xFA][arg1][arg2][arg3][len][payload...][chk][0xAF]`, where
`chk` is a plain XOR over `[0xFA .. last payload byte]` inclusive —
confirmed byte-exact against two frames captured live off the real
device.

**What's on the other end of `ttyS2` is not known.** 4800 baud rules out
`MCUAdapter_BoxP300` itself (confirmed 38400 baud on `ttyHS0`), so this
is likely either a separate physical peripheral reusing the same
packaging function (a steering-wheel-control CAN/serial bridge is one
candidate, see `docs/1.3_MCU_ADAPTERS.md`'s Catalogue), or a secondary port
on the same MCU.

```sh
# Replays the two known-good captured frames, then listens for more
mcu-handshake --ttys2

# Verbose, and skip the replay (pure passive listening)
mcu-handshake --ttys2 --no-hello -v

# Override port/baud if needed
mcu-handshake --ttys2 -p /dev/ttyS2 -b 4800 -v
```

As with `ttyHS0`, toggle a physical input (steering wheel buttons are the
prime suspect here) while listening to see if it produces traffic, and
correlate frame content with the action to start decoding `arg1`/`arg2`/
`arg3`'s real meaning.

### Still open
- `showApp(mode=0xCC)` sends a fourth real frame (`cmd=0x82`, a
  *different* 4-byte payload `02 0B 00 00` than the `onModeAppChanged`
  one above) — not sent by this tool, since `showApp` is never called
  from inside `libMcuCenter.so` itself (it's driven externally via
  vtable dispatch from `MsnCoreApp`'s own UI/navigation code), so its
  real trigger condition is unconfirmed. Structurally, `showApp`'s
  `mode=0x203` case dispatches through a live UI object's vtable to
  what looks exactly like a `show()`/`raise()` vs `hide()`/`lower()`
  pair — strong circumstantial evidence `showApp` really is "make the
  Limcet UI visible," which would tie it to both the AUX/long-press
  switch and auto-switching when a phone connects. If the three-frame
  startup sequence above doesn't trigger the touch switch, tracing
  `MsnCoreApp`'s own binary (`/usr/bin/MsnCoreApp`) by vtable offset to
  find `showApp`'s real caller is the next step.
- The real-world semantic meaning of `msnAppStateChange`'s bit-flag
  values (which bit = phone connected vs. ACC vs. reverse, etc.) is
  still unresolved — `libMcuCenter.so` alone doesn't reveal who calls
  it or why; would need either `MsnCoreApp`'s central event dispatcher
  traced by vtable offset, or a live `/dev/ttyHS0` capture per
  `docs/1.3_MCU_ADAPTERS.md`'s existing Method A/B procedures while
  triggering each real-world event separately.
