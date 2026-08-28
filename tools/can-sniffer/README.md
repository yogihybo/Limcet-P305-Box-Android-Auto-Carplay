# can-sniffer

Real-time CAN1 sniffer for the Prado companion STM32F105, driven entirely
over SWD via OpenOCD's Tcl RPC — no application or bootloader code execution
involved. Companion tool for `docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md`, which
documents every step this script automates as manual `mww`/`mdw` commands
first, and explains the reasoning (RDP protects flash content, not
peripheral-register access; CAN1 receives frames autonomously in hardware
once configured, no CPU code needs to run to keep it working).

## Why this exists

Manually typing individual `mdw` commands into the OpenOCD telnet console
cannot catch a momentary event — a quick steering-wheel button press, a
brief reverse-gear transition — because each interactive round trip takes
seconds. This script polls CAN1's RX FIFO in a tight loop and logs every
real frame with a high-resolution timestamp, so events can be correlated
against real-world actions after the fact instead of needing to catch them
live by hand.

In practice this matters less than it might sound for *periodic status*
signals (most vehicle CAN messages, including "button currently held",
repeat every 10-100ms while the condition is true, not just once) — but it
matters a lot for anything closer to a genuine single-shot event, and it's
strictly better data either way: continuous logging plus your own real-time
notes ("pressed volume-up around the 45s mark") beats trying to catch
something live on a slow interactive console.

## Usage

```
python3 can_sniffer.py [--host localhost] [--port 6666] [--log capture.csv]
```

Requires OpenOCD already running with its Tcl RPC server reachable (same
setup used successfully elsewhere in this project) and the target connected
via SWD. Connecting forces a reset on this hardware — expected, not an
error. We only ever get bootloader-level context afterward, which is fine:
this script configures CAN1 entirely itself and never needs the application
to run.

The script prints every captured frame to stdout live:
```
[    12.487] id=105  DAT dlc=2 data=0400
[    12.501] id=28A  DAT dlc=4 data=00120034
```
and, with `--log`, appends the same data as CSV (`t_seconds,can_id,ide,rtr,dlc,data_hex`)
for later analysis (e.g. diffing successive rows for a given ID to find
which bits/bytes change when a specific real-world action happens).

## What it does, step by step

1. `reset halt` — connects and confirms we're in the bootloader (prints the
   halted PC; sanity-check against `0x08004xxx`+, the app region, which we
   should NOT be in).
2. Sets `DBGMCU_CR`'s watchdog-freeze bits (`DBG_IWDG_STOP`/`DBG_WWDG_STOP`,
   already used successfully elsewhere this project) and explicitly clears
   `DBG_CAN1_STOP` — a real precaution against CAN1 silently not receiving
   anything while the core is halted, checked rather than assumed.
3. Enables `CAN1`/`GPIOA`/`AFIO` peripheral clocks.
4. Configures `PA11` (RX, input pull-up) / `PA12` (TX, AF push-pull).
5. Enters CAN1 initialization mode, programs 500 kbit/s bit timing (the
   exact `BTR` value already used in `hardware/MCU/source/src/can_driver.c`,
   not re-derived), opens filter 0 to accept every ID into FIFO0, exits
   initialization mode.
6. Polls `CAN1->RF0R` in a tight loop; on a pending frame, reads the FIFO0
   mailbox registers, decodes ID/IDE/RTR/DLC/data (same decode
   `can_driver.c`'s own `CAN1_RX0_IRQHandler()` already uses), releases the
   FIFO slot, and logs.

## Recovery

Ctrl+C stops the poll loop and closes the OpenOCD connection cleanly. Same
standing recovery for the target itself as every other SWD procedure in this
project: fully disconnect OpenOCD, then a real physical power cycle. Do not
rely on `resume`.

## Real caveats, not glossed over

- Whether the physical CAN transceiver chip is powered/enabled independent
  of anything this MCU controls isn't confirmed. If `RF0R` never shows a
  pending frame at all despite the vehicle clearly generating CAN traffic,
  that's the likely explanation, not necessarily a bug in this script.
- `DBG_CAN1_STOP`'s bit position (14) is from general STM32F1
  connectivity-line reference knowledge, not independently re-verified
  against a fetched ST datasheet this session — same honesty caveat this
  project applies elsewhere when a primary source couldn't be fetched.
- The poll loop has no artificial delay (`continue` immediately on an empty
  FIFO) to maximize catch rate, at the cost of spamming OpenOCD with `mdw`-
  equivalent Tcl RPC calls continuously. This is deliberate, not an
  oversight — but means the script will use a full CPU core and generate
  continuous SWD/USB traffic for as long as it runs.
