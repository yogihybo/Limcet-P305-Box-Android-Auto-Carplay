# Real Prado CAN Bus Sniffing via SWD — No App Code, No Extra Hardware

**Status**: plan only, not yet executed. Directly addresses this project's
long-standing open question — real Toyota Prado CAN IDs for steering wheel
controls / steering angle / gear — without needing a separate dev board or
a physical bus tap, using the actual MCU already wired into the real vehicle.

## Why this works despite RDP and the bootloader-only constraint

Same principle as `MCU_VIDEO_RELAY_SWD_TEST_PLAN.md`: RDP Level 1 protects
flash content, not the debugger's direct AHB-AP access to peripheral
registers. The resident bootloader never touches CAN1 at all (it only needs
`USART2` for YMODEM) — but that's not a blocker, because **we don't need any
CPU code to run CAN1 at all**. Once the peripheral is configured (clock
enabled, pins set, bit timing programmed, filters open, taken out of
initialization mode), the CAN controller receives and stores frames into its
own hardware FIFO **autonomously** — this is a real, dedicated hardware state
machine, not something driven by repeatedly-executing firmware. We only need
the CPU (or here, the debugger acting as one) to periodically *read* the FIFO,
which `mdw` does perfectly well.

This means: connect via SWD (forces the usual reset, same as every other plan
in this project), configure CAN1 entirely ourselves via `mww`, then just poll
`CAN1->RF0R` / the FIFO mailbox registers in a loop while the vehicle is
actually being driven/used, and see **real Prado CAN traffic** — completely
sidestepping both RDP and the "we've never captured a real dump" problem this
project has had since the beginning.

## Real caveat, stated up front

Whether the physical CAN transceiver chip (the differential-bus interface
between `CAN1`'s TX/RX logic-level pins and the actual vehicle bus wiring) is
powered/enabled independent of anything this MCU or the ArkMicro SoC controls
is **not confirmed**. Most simple transceivers (TJA1050-class) have no enable
pin and are always active given power, but this hasn't been checked against
a schematic. If the transceiver turns out to need an enable signal this
sequence doesn't provide, CAN1 would configure cleanly but never actually see
bus traffic — a real, honest possibility to watch for (if `RF0R` never
increments at all even with the vehicle clearly generating CAN traffic,
that's the likely explanation, not necessarily a sequencing mistake).

## Register addresses used

All addresses computed directly from `hardware/MCU/source/include/stm32f105.h`'s
own `CAN_TypeDef` layout, not assumed from a generic reference:

| Register | Address | Purpose |
|---|---|---|
| `RCC->APB1ENR` | `0x4002101C` | bit 25 = `CAN1EN` |
| `RCC->APB2ENR` | `0x40021018` | bits 0,2 = `AFIOEN`, `IOPAEN` |
| `GPIOA->CRH` | `0x40010804` | PA11 (RX, input) / PA12 (TX, AF push-pull) config |
| `GPIOA->ODR` | `0x4001080C` | PA11 pull-up enable |
| `CAN1->MCR` | `0x40006400` | mode control (sleep/init/normal) |
| `CAN1->MSR` | `0x40006404` | status (init-ack bit) |
| `CAN1->BTR` | `0x4000641C` | bit timing (500 kbit/s config) |
| `CAN1->RF0R` | `0x4000640C` | FIFO0 pending-message count (bits `[1:0]`) |
| `CAN1->sFIFOMailBox[0].RIR` | `0x400065B0` | received frame: ID/IDE/RTR |
| `CAN1->sFIFOMailBox[0].RDTR` | `0x400065B4` | received frame: DLC (bits `[3:0]`) |
| `CAN1->sFIFOMailBox[0].RDLR` | `0x400065B8` | received frame: data bytes 0-3 |
| `CAN1->sFIFOMailBox[0].RDHR` | `0x400065BC` | received frame: data bytes 4-7 |
| `CAN1->FMR` | `0x40006600` | filter-init mode |
| `CAN1->FM1R` | `0x40006604` | filter mode (mask vs. list) |
| `CAN1->FS1R` | `0x4000660C` | filter scale (16-bit vs. 32-bit) |
| `CAN1->FFA1R` | `0x40006614` | filter FIFO assignment |
| `CAN1->FA1R` | `0x4000661C` | filter activation |

Bit-timing value for 500 kbit/s (assuming APB1 = 36 MHz, matching this
project's own `can_driver.c`): `BTR = 0x004B0003` (`SJW=0, BRP=3, TS1=11,
TS2=4` giving 18 Tq/bit — same value the clean-room source already uses,
not re-derived here, just reused since it's already real).

## Procedure

### Phase 1 — Connect

```
reset halt
reg pc                          # sanity check, same as the GPIO plan
```

### Phase 2 — Enable clocks

```
mdw 0x4002101C
```
`NEW = OLD | 0x02000000`   (set `CAN1EN`)
```
mww 0x4002101C <NEW>
mdw 0x40021018
```
`NEW = OLD | 0x5`   (set `AFIOEN` + `IOPAEN`)
```
mww 0x40021018 <NEW>
```

### Phase 3 — Configure PA11 (RX, input pull-up) and PA12 (TX, AF push-pull)

```
mdw 0x40010804
```
`NEW = (OLD & ~(0xF<<12) & ~(0xF<<16)) | (0x8<<12) | (0xB<<16)`
```
mww 0x40010804 <NEW>
mdw 0x4001080C
```
`NEW = OLD | (1<<11)`   (PA11 pull-up)
```
mww 0x4001080C <NEW>
```

### Phase 4 — Enter CAN1 initialization mode

```
mdw 0x40006400
```
`NEW = (OLD & ~(1<<1)) | (1<<0)`   (clear `SLEEP`, set `INRQ`)
```
mww 0x40006400 <NEW>
```

Poll until acknowledged (repeat `mdw 0x40006404` until bit 0 — `INAK` — reads 1):
```
mdw 0x40006404
```

### Phase 5 — Program bit timing (500 kbit/s)

```
mww 0x4000641C 0x004B0003
```

### Phase 6 — Open the filter to accept every ID into FIFO0

```
mdw 0x40006600
```
`NEW = OLD | (1<<0)`   (`FINIT`)
```
mww 0x40006600 <NEW>
mdw 0x4000661C
```
`NEW = OLD & ~(1<<0)`   (deactivate filter 0 while configuring)
```
mww 0x4000661C <NEW>
mdw 0x4000660C
```
`NEW = OLD | (1<<0)`   (32-bit scale)
```
mww 0x4000660C <NEW>
mdw 0x40006604
```
`NEW = OLD & ~(1<<0)`   (mask mode, not list mode)
```
mww 0x40006604 <NEW>
```

Set filter 0's own ID/mask registers to accept-all (mask = 0, matches
everything) — these are `CAN1->sFilterRegister[0].FR1`/`FR2`, at
`0x40006400 + 0x240` and `+0x244`:
```
mww 0x40006640 0x00000000
mww 0x40006644 0x00000000
```

Re-activate filter 0 and exit filter-init mode:
```
mdw 0x4000661C
```
`NEW = OLD | (1<<0)`
```
mww 0x4000661C <NEW>
mdw 0x40006600
```
`NEW = OLD & ~(1<<0)`
```
mww 0x40006600 <NEW>
```

### Phase 7 — Exit initialization mode, enter normal mode

```
mdw 0x40006400
```
`NEW = OLD & ~(1<<0)`   (clear `INRQ`)
```
mww 0x40006400 <NEW>
```

Poll until acknowledged (repeat until bit 0 of `0x40006404` reads 0):
```
mdw 0x40006404
```

CAN1 is now live and should be autonomously receiving real bus traffic into
FIFO0, entirely in hardware.

### Phase 8 — Poll and record real frames

```
mdw 0x4000640C
```
If the low 2 bits are nonzero, a real frame is pending — read it:
```
mdw 0x400065B0    # RIR
mdw 0x400065B4    # RDTR
mdw 0x400065B8    # RDLR
mdw 0x400065BC    # RDHR
```

Then release the FIFO slot (write `RFOM0`, bit 5, of `RF0R`) so the next
frame can land:
```
mww 0x4000640C 0x00000020
```

Repeat. **This is naturally a tight polling loop, and manual interactive
typing genuinely cannot catch a momentary event** (a quick steering-wheel
button press, a brief gear transition) — each hand-typed round trip takes
seconds, far slower than the event itself. `tools/can-sniffer/can_sniffer.py`
automates exactly this: it performs the whole Phase 1-7 sequence above
itself, then polls `RF0R` in a tight loop and logs every real frame with a
high-resolution timestamp to a CSV file, so events can be correlated against
real-world actions (button presses, gear changes, wheel turns) after the
fact instead of needing to catch them live. See its own `README.md` for
usage and the full list of real caveats (transceiver power/enable
uncertainty, the `DBG_CAN1_STOP` precaution it takes, etc.). Run it for as
long as the vehicle is being operated (ignition on, engine running, actually
driven/reversed/steering-wheel-buttons-pressed) to capture real traffic.

### Decoding a captured frame

- `RIR` bit 2 (`IDE`): 0 = standard 11-bit ID, 1 = extended 29-bit ID.
- If `IDE`=0: standard ID = `RIR` bits `[31:21]`.
- `RIR` bit 1 (`RTR`): 0 = data frame, 1 = remote frame.
- `RDTR` bits `[3:0]`: DLC (0-8).
- `RDLR` = data bytes 0-3 (byte 0 in bits `[7:0]`, byte 1 in `[15:8]`, etc.).
- `RDHR` = data bytes 4-7, same byte ordering.

Same decode this project's own `can_driver.c` `CAN1_RX0_IRQHandler()` already
uses — reused here, not re-derived.

## What this resolves

Real, direct, on-vehicle confirmation of this project's single biggest open
question: the actual CAN IDs and byte-level encoding for steering wheel
controls, steering angle, and gear/reverse on this specific Prado — replacing
the current best-available evidence (the real firmware's own built-in
"Mode 1/2/3" profile guesses, cross-referenced against `opendbc`) with a
genuine capture. Press a steering wheel button, note which ID/byte changes;
engage reverse, note which ID/bit flips; turn the wheel, note which ID's
value tracks the angle. This is exactly the kind of ground truth
`vehicle_profiles.c`'s own comments have been flagging as missing all
session.

## Recovery

Same as every other SWD plan in this project: fully disconnect OpenOCD, then
a real physical power cycle. Do not rely on `resume`.
