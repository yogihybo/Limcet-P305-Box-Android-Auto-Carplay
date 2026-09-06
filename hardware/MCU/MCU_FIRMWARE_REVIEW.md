# MCU Firmware Review — `can_app.bin` (STM32F105RBT6)

> ⚠️ **`can_app.bin` is NOT the firmware installed on the device.** It is a
> **Volvo-profiled candidate** (`DCn32-VOLVO-V2.10-20240909`) that ships as a USB
> update *payload* in this folder. The STM32 in the actual Prado unit runs the
> **stock Toyota Prado MCU firmware**, which is **not in this repo** (it would
> have to be dumped via SWD — there is no serial read-back, see §7). That Toyota
> firmware is what correctly decodes the Prado's CAN (reverse, illumination, SWC).
> **Do not flash `can_app.bin` onto the Prado** — it would overwrite the working
> Toyota firmware with a Volvo profile. This document analyses the Volvo candidate
> file; the *mechanisms* it describes (two-tier settings/profile, CAN TX/RX, the
> update path) apply to the Toyota firmware too, since both are the same `DCn32`
> codebase with a different vehicle profile compiled in — only the CAN
> tables/filters differ.

Analysis of the vehicle-side I/O co-processor firmware for the Limcet Box P306.
The MCU is an **STM32F105RBT6** (ARM Cortex-M3, connectivity line, 128 KB flash /
64 KB SRAM) that handles CAN bus, steering-wheel / panel keys, reverse and
ACC-IGN signals, and drives the Feasycom BT module. It talks to the ARK1668 SoC
over `/dev/ttyHS0`.

Cross-references: [`../../docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`](../../docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md)
(this project's authoritative, continuously-updated MCU firmware findings
doc -- check there for anything security-relevant or for the latest
correction on a specific address/command; this doc's own content was
cross-checked against it 2026-08-30 and found still accurate, including
the 15-slot CAN RX ring size below, the `CMD 0xE1` bootloader-trigger
mechanism, and the write-only/no-read-back update-flow conclusion),
[../docs/historical/1.3_MCU_ADAPTERS.md](../../docs/historical/1.3_MCU_ADAPTERS.md),
[../docs/1.2_CANBUS.md](../../docs/1.2_CANBUS.md), [../docs/3.2_SECURITY_REVIEW.md](../../docs/3.2_SECURITY_REVIEW.md),
`../Prado firmware dump/mtd6_rootfs/usr/lib/libMcuCenter.so` (SoC-side driver).

---

## 1. Files in this directory

| File | Size | Notes |
|------|------|-------|
| `can_app.bin` | 31,996 B | The MCU firmware **payload** (raw app image, links at 0x08004000). |
| `auto_upgrade.txt` | 0 B | **The MCU update trigger flag** — its presence starts the update (see §5.0). |

Together these two files **are a ready-to-deploy USB/SD MCU update package** for
this unit: `auto_upgrade.txt` is the trigger the head unit scans for, and
`can_app.bin` is the firmware it streams to the MCU. Both filenames are literal
strings inside the SoC driver `libMcuCenter.so` (confirmed), adjacent to
`onStartUpdateMCU()` / `find update file:` / `not found update file!`.

`can_app.bin` is the *application half only*; a resident bootloader that is
**not** in this repo lives below it in flash (§4).

---

## 2. Identity & memory map

Confirmed by parsing the vector table and by pointer-range analysis of the image.

| Property | Value |
|---|---|
| Core | ARM Cortex-M3 (STM32F105RB, connectivity line) |
| Image size | 31,996 B (~31 KB of 128 KB flash) |
| **Link/load address** | **0x08004000** |
| Initial SP | 0x20002DC0 |
| Reset handler | 0x08004239 (Thumb) |
| Flash pointers observed | 0x08004000 … 0x0800F04F |
| SRAM pointers observed | 0x2000xxxx (data/bss/stack) |

**Two-stage layout.** Everything links at 0x08004000 with a clean, empty 16 KB
gap below it. That gap (0x08000000–0x08004000) holds a separate **resident
bootloader**, which is why the application starts at 0x08004000. That bootloader
is what performs firmware updates (§5). It never leaves the chip and is **not in
this repo** — the repo only contains Cortex-A SoC boot binaries
(Nboot/Stepldr/u-boot/ARKSDLDR), none of which are the STM32 bootloader.

Reset handler is a textbook `SystemInit(); main();`:
```
0x08004238  ldr r0,[pc]; blx r0     ; SystemInit / lib init
0x0800423c  ldr r0,[pc]; bx  r0     ; -> main()
```

---

## 3. Interrupt handlers & peripherals

The 64-entry IRQ table points almost everything at a common dummy trap
(`0x08004253`). The live handlers:

| IRQ (STM32F105) | Handler | Role |
|---|---|---|
| CAN1_RX0 | 0x08007064 | Fills a 15-slot × 20-byte software ring buffer from CAN FIFO0 |
| USART2 | 0x08007298 | Framed byte protocol (RX state machine + TX ring) |
| USART3 | 0x0800755c | Framed byte protocol |
| UART4  | 0x08007780 | Framed byte protocol |
| UART5  | 0x08007a9c | Framed byte protocol |
| USART1 | — | `bx lr`, unused |

Peripheral register references in the image (peripheral base → hit count):
GPIOB ×22, GPIOC ×13, GPIOA ×12, RCC ×11, **IWDG ×6** (independent watchdog is
enabled), **bxCAN1 ×5 / bxCAN2 ×4**, USART2/3 + UART4/5, FLASH ×3, GPIOD ×2, AFIO, PWR.

Only **CAN1** raises RX interrupts; CAN2 registers are touched but no CAN2 IRQ
vector is populated (filter/master use or polled).

### 3.1 UART framing protocol
The USART handlers run a **stateful binary packet parser** — a state byte in
SRAM (`0x2000005a`) walks cases 0/1/2/3 = header → length → payload → checksum,
with a TX ring buffer drained on TXE. This is the head-unit ↔ MCU protocol. One
UART is the SoC link (`/dev/ttyHS0`); another drives the Feasycom BT module.
`/dev/ttyHS0` on the SoC side is a raw PL011 UART (see
`../linux-arkmicro Reference/.../mcu_serial.c`); all framing is in software above it.

**2026-08-13 — cross-confirmed from this side, not just `libMcuCenter.so`.**
Everything `tools/mcu-handshake` and this doc previously knew about the wire
framing came from the SoC-side driver (`libMcuCenter.so`'s
`MCUAdapter_BoxP300`) — never from the MCU's own firmware. Imported
`can_app.bin` into Ghidra as a raw ARM binary (`ARM:LE:32:Cortex`, base
`0x08004000`, Thumb context forced across the app region since Cortex-M3 is
Thumb-only and Ghidra's raw-binary loader defaults to ARM mode with no vector
table to signal otherwise) and decompiled the real USART2 ISR at `0x08007298`.
It is a clean, complete match to the framing already documented:

```
if (TXE pending) {                       // ring-buffer TX drain (already documented)
    ...
}
if (RXNE pending) {
    byte = read data register
    switch (state @ 0x2000005a) {
    case 0:  if (byte == 0x2E) state = 1;                          // header
    case 1:  msg[msgIdx].cmd = byte; state = 2;                    // cmd byte
    case 2:  if (byte < 0x1c && byte != 0) {                       // length byte
                 msg[msgIdx].len = byte;
                 remaining = byte + 3;   // len + cmd + checksum framing overhead
                 writeIdx = 2; state = 3;
             } else state = 0;           // invalid length -> resync
    case 3:  msg[msgIdx].payload[writeIdx++] = byte;
             if (writeIdx == remaining) {
                 msgIdx = (msgIdx + 1) % 8;   // 8-slot ring, 0x1e (30) bytes/slot
                 state = 0;
             }
    }
}
```

This independently confirms, from the opposite end of the wire, that
`[0x2E][cmd][len][payload...][checksum]` (already `tools/mcu-handshake`'s
assumption) is exactly right — not inferred from the SoC side alone. The
8-slot × 30-byte ring buffer this fills (`msg[msgIdx].cmd` at slot+4,
`.len` at slot+5, payload/checksum following) is a real, previously-unknown
structural detail: completed frames queue up to 8 deep before whatever
processes them (see below) has to catch up.

### 3.1b Auxiliary UART protocols (USART3, UART4, UART5)

Direct disassembly of the remaining UART handlers revealed the exact protocols and roles of the other serial interfaces:

- **USART3 (`0x0800755c`, PB10 TX / PB11 RX, 9600 baud)**:
  - Dedicated link to the onboard Bluetooth module (Feasycom / Blueware).
  - Handles ASCII newline-terminated AT commands: `"AT+AUDROUTE=1\r\n"` / `"AT+AUDROUTE=2\r\n"` triggered on audio routing switches, and `"AT+UPGRADE\r\n"` on camera update events.
  - Serves as the verbatim bidirectional relay for SoC `CMD 0x87`. Inbound bytes on `USART3_IRQHandler` (`0x08006ed3`) are packaged into `0x2E` packets and forwarded to the SoC.

- **UART4 (`0x08007780`, PC10 TX / PC11 RX, 9600 baud) & UART5 (`0x08007a9c`, PC12 TX / PD2 RX, 9600 baud)**:
  - Both peripherals are clocked via `RCC->APB1ENR` bits 19/20 and operate at 9600 baud 8N1 (`0x0800771c`, `0x080079b8`: `mov.w r0, #9600`).
  - Share a common SRAM structure at `0x20001365` running a clocked state machine:
    - **Framing**: Sync byte `0x55` followed by a Command byte.
    - **`0x20` (Outbound Dynamic Vehicle / SWC Status, 5 bytes)**: Byte 0 sent immediately; bytes 1–4 are clocked out one by one as the peer sends subsequent bytes. Backed by `struct + 0x21` (`0x20001386`), dynamically refreshed by `0x0800B8A0` using CAN key/alert data from `struct + 4` and `struct + 8`.
    - **`0x32` (Outbound Hardware Identity, 9 bytes)**: Byte 0 sent immediately; bytes 1–8 clocked out. Emits literal flash string `"   cD31\0\x93"` from `0x0800BBCD`.
    - **`0x50` (Inbound Buffer, 3 bytes)**: Safely bounded write into `struct + 0x4E`.
    - **`0xD3` (Inbound Buffer, 9 bytes)**: Safely bounded write into `struct + 0x3F`.
    - **`0xD6` (Inbound Buffer, 9 bytes)**: Safely bounded write into `struct + 0x5D`.
  - **UART4 $\leftrightarrow$ UART5 Bridging**: UART5 acts as a complementary relay to UART4 (`0x08007b00`–`0x08007c80`), re-transmitting data received on UART4 (`0xD3`/`0x50`/`0xD6`) outbound over UART5, and vice versa.

### 3.1c Command dispatch table — found, complete, 9 real commands

**Found via a different route than Ghidra's CFG.** Tracing `main()` forward
through Ghidra's headless decompiler hit diminishing returns behind several
layers of generic Cortex-M/newlib startup boilerplate. Switched tools instead:
used `capstone` (Python) to linearly disassemble the raw binary directly (it
handles Thumb/Thumb-2 mixed-length instructions and PC-relative literal pools
correctly without Ghidra's function-boundary/context-register friction), then
searched the whole binary for every `BL`-encoded call targeting the ring-buffer
"pop next message" function (`0x0800720c`, found in §3.1's ISR trace via a
`ChannelManager`-parallel structure: `is_empty()`/`reset()`/`pop_message()`
helpers at `0x080086a0`/`0x080086e0`/`0x0800720c`). Exactly **one call site**
exists, at `0x080045e6`, inside a real dispatcher function (prologue at
`0x080045d8`):

```c
// (reconstructed from raw disassembly, not decompiler output)
status = pop_message(&msg, 0);
if (status != 0) return;               // ring buffer empty
if (!some_check())  return;            // FUN_08007dc4 -- gating condition, not decoded
for (i = 0; i < 9; i++) {
    if (cmd_table[i].cmd == msg.cmd) { // 8-byte stride: {u8 cmd; u8[3] pad; u32 handler}
        cmd_table[i].handler(...);
        break;
    }
}
```

The table lives at `0x0800b9e4`, 9 entries × 8 bytes (`cmd` byte at `+0`,
handler function pointer at `+4`). All 9 extracted directly from the binary:

| cmd | handler | what it does |
|---|---|---|
| `0x81` | `0x080088b4` | Calls a generic "reset state slot" helper (`FUN_08006228(val=0, idx)`) four times with idx = 12, 1, 8, 10. Resets four internal state-table entries to 0. **Confirmed by `tools/mcu-handshake`'s own `onInited()` frame.** No GPIO/hardware register touched. |
| `0x82` | `0x08008bd4` | Reads `msg.payload[2]`; if `==1`, sets internal `{mode=4, type=1}` and calls the same reset-helper on idx 1; else sets `{mode=1, type=2}`. **The exact payload byte at offset 2 decides which branch fires** — worth double-checking `tools/mcu-handshake`'s 9-byte `onModeAppChanged` payload actually has `1` at that position if mode=4 is the intent. |
| `0x84` | `0x08008808` | Reads `msg.payload[3] & 0xf` as a "type" value (bails if ≥6); dedups against the previous type; on a real change, only types `0` and `3` trigger real action (`FUN_080058a4(0)` / `FUN_080058a4(1)` respectively, most likely an audio-route select given the doc's own `AT+AUDROUTE=1/2` finding) — types 1/2/4/5 are silent no-ops. |
| `0x85` | `0x08008ba8` | Copies `payload[3..5]` into internal state, calls a second generic setter `FUN_080062fc(idx=5, val=3)`. |
| `0x87` | `0x080087a0` | Copies `payload[2..5]` into a stack buffer, calls `FUN_08004278` (undecoded), stores the same 4 bytes into internal state at offsets 7-10, then calls `FUN_08007600(idx=0xd, ...)` — same helper `0x84`/`0xa0` use with different index constants, likely a generic "notify/apply subsystem N" dispatcher (candidate: triggers one of the documented `AT+*` commands to the Feasycom BT module). |
| `0x88` | `0x0800893c` | Copies 8 bytes between two buffers, then packs two separate big-endian-ish 32-bit words out of `payload[2..5]` and `payload[6..9]` — receiving some 8-byte value from the SoC (candidate: a timestamp or counter, not yet identified further). |
| `0xa0` | `0x080089d8` | A rich `switch` (`tbb` jump table) on `payload[2]` with up to 18 cases — sets a state byte at offset `0x3b` to the case value and, for several cases, calls the same `FUN_08007600` "apply" helper with different index constants (`0xc` seen). By far the largest/most complex handler of the 9 — a genuine multi-mode command, not yet fully enumerated case-by-case. |
| `0xe1` | `0x080088e0` | **Just calls `FUN_080045c4`**, which writes the literal `0x5555aaaa` to SRAM address `0x20004004`, then spins forever (`b $`). Not a hardware reset-register write (`0x20004004` is plain SRAM, not `NVIC_AIRCR`/IWDG territory) — this is the classic "leave a magic value in a fixed RAM location for the bootloader to check after reset" pattern. Since the loop never feeds the independent watchdog (already confirmed enabled, §3), IWDG will force an actual hardware reset shortly after. **Strong candidate for a software-triggerable "enter bootloader / reboot" command** — if the resident bootloader (§4, not in this repo) checks this exact magic value at this exact address on boot, this could be an alternative to the `auto_upgrade.txt` USB-trigger flow (§5) for entering update mode. Not confirmed without either the bootloader binary or a live hardware test (send `cmd=0xe1` and see if the unit reboots / enters update mode). |
| `0xff` | `0x080088e8` | Another `payload[2]`-keyed switch (values 6,7,8,9,0x7f recognized); only value `6` (and the shared fallthrough for several cases) calls the reset-helper (`FUN_080062fc(idx=0xc, val=0)`) — most other values are no-ops in this build. |

**How this was found — technique note for future MCU/CAN-app reverse-engineering
in this repo:** Ghidra's raw-binary loader + headless CFG-chasing is fine for
finding a KNOWN address's function (as §3's ISR trace shows), but tracing
`main()`'s call graph forward through generic startup code via headless
round-trips has poor time-to-value. `capstone`'s linear/recursive disassembly
plus a targeted "find every BL to address X" scan (given at least one known
callee address, found here via data-xrefs to the ring buffer) got to the real
answer far faster. Worth reaching for first next time, rather than more Ghidra
script iterations.

### 3.1d Which incoming CAN frames generate which commands — found, complete

**2026-08-14, at explicit request ("is it possible to work out what the input
CAN frames are that generate the commands").** Same `capstone` technique as
§3.1c, applied in the opposite direction: instead of searching for callers of
a known UART function, searched for every literal-pool load of the **CAN RX
ring's base address** (`0x200002bc`, derived from the CAN1_RX0 ISR at
`0x08007064` — see §3.1b below for the ISR itself). 250+ hits, spanning
`0x08007f1c`-`0x0800b7ee` — a genuinely large amount of code, much bigger than
the 9-command UART dispatcher.

**The real consumer function** (`0x08007f04`, called from the main loop) pops
one frame off the 15-slot CAN RX ring (read/write index bytes at `+0x12c`/
`+0x12d`, same struct the ISR fills), extracts either `StdId` or `ExtId`
(picked via an IDE flag byte in the ring slot), then dispatches by CAN ID
through **one of three separate lookup tables**, selected by a `mode` byte
(`ring_struct+0x36`, same base as the write/read indices):

```c
uint32_t id = frame.ide ? frame.ExtId : frame.StdId;
switch (mode) {                    // mode read from ring_struct+0x36
case 3: table = mode3_table; count = 9;  break;
case 2: table = mode2_table; count = 10; break;
case 1: table = mode1_table; count = 9;  break;
// mode 0 (or >3): no CAN dispatch happens at all
}
for (i = 0; i < count; i++)
    if (table[i].can_id == id) { table[i].handler(); break; }
// unconditionally, regardless of match: re-copy + CAN_Transmit() the frame,
// then advance the ring's read index (wrap at 15)
```

Each table is an 8-byte-stride array (`u32 can_id; u32 handler_ptr`) —
structurally the same layout as the UART command table in §3.1c, just keyed
on CAN ID instead of a protocol byte. All 28 entries across the three tables,
extracted directly from the binary (`mode3_table@0x0800bae8`,
`mode2_table@0x0800bb30`, `mode1_table@0x0800bb80`):

| mode | CAN ID | handler | | mode | CAN ID | handler |
|---|---|---|---|---|---|---|
| 3 | `0x03a` | `0x08009898` | | 2 | `0x020` | `0x0800ad80` |
| 3 | `0x04d` | `0x08009efc` | | 2 | `0x060` | `0x0800b7d8` |
| 3 | `0x135` | `0x0800975c` | | 2 | `0x110` | `0x0800b548` |
| 3 | `0x168` | `0x08009dd0` | | 2 | `0x220` | `0x0800af60` |
| 3 | `0x214` | `0x08009960` | | 2 | `0x170` | `0x0800adf0` |
| 3 | `0x110` | `0x0800990c` | | 2 | `0x240` | `0x0800aca8` |
| 3 | `0x2c3` | `0x08009690` | | 2 | `0x1a0` | `0x0800b0c4` |
| 3 | `0x160` | `0x080094a8` | | 2 | `0x080` | `0x0800b4f0` |
| 3 | `0x000` | *(none — unused slot)* | | 2 | `0x090` | `0x0800ae44` |
| | | | | 2 | `0x3a0` | `0x0800aca4` |
| 1 | `0x0f5` | `0x0800a7bc` | | | | |
| 1 | `0x2d5` | `0x0800a5a8` | | | | |
| 1 | `0x28a` | `0x0800a680` | | | | |
| 1 | `0x215` | `0x0800aa4c` | | | | |
| 1 | `0x185` | `0x0800a8e4` | | | | |
| 1 | `0x245` | `0x08009fc4` | | | | |
| 1 | `0x195` | `0x0800a2f0` | | | | |
| 1 | `0x105` | `0x0800a938` | | | | |
| 1 | `0x035` | `0x0800abc8` | | | | |

**One handler fully decoded, closing the loop end-to-end.** `mode=1, CAN ID
0x105`'s handler (`0x0800a938`) reads bits `0x80` and `0x4` out of the CAN
frame's data byte at slot offset `+0xc`, and — gated on those bits plus an
internal flag byte — calls the exact same generic state-setter
(`FUN_08006228`, offset `0x08006228`) that `cmd=0x81`'s **UART** handler
(§3.1c) also uses, writing `state[7] = 0x4101` or `state[7] = 0x4001`
depending on which bit pattern matched. Those two values differ by a single
bit (`0x100`) — exactly the shape of a key-down/key-up toggle on an otherwise
fixed key code. This is the real, concrete link the whole codebase was built
around: **a specific CAN frame -> bit-pattern match -> generic state-slot
write -> (the same slot mechanism `cmd=0x81`/`0xff`'s UART handlers reset)
-> eventually marshaled into an outgoing UART frame to the SoC.** The other
27 handlers weren't individually decoded this pass (see below), but all share
the same table structure and very plausibly the same state-slot-write pattern
for the ones that map to steering-wheel/panel key events.

**Caveat that matters: these 28 CAN IDs are Volvo-profile IDs, not Toyota
ones.** This whole file is confirmed (top of this document) to be the
`DCn32-VOLVO` build, not the real firmware flashed on the Prado. None of these
28 IDs match the Prado's own documented CAN IDs (`docs/1.2_CANBUS.md`'s SWC at
`0x3C4`, for instance, appears nowhere in any of the three tables) — so this
specific table content tells you nothing about decoding the *real* Toyota
bus. What **does** transfer directly: the *mechanism* — a mode-selected,
CAN-ID-keyed dispatch table feeding a shared generic state-setter that's also
what the SoC-facing UART command handlers use — since (per this document's
own opening warning) the real Toyota firmware is the same `DCn32` codebase
with only the vehicle profile/tables swapped, not a different architecture.
If the real Toyota `can_app.bin` is ever obtained, this exact
`capstone`-based technique (find the CAN RX ring's literal-pool xrefs, locate
the `mode`-selected table trio, dump the 8-byte-stride entries) will find its
real Toyota-specific CAN-ID-to-key mapping just as fast.

**Not done this pass** (real, bounded follow-up work if ever needed): decode
the remaining 27 handlers individually (mechanical repeat of the one worked
example above); confirm what selects `mode` at runtime (likely a
config/product-variant setting, not investigated); confirm the "unconditional
CAN_Transmit() of the just-received frame" behavior noted in the dispatcher's
tail — worth understanding whether that's a genuine bus relay/echo or
something narrower, since it happens for every dispatched (and undispatched)
frame regardless of table match.

### 3.1b CAN — the MCU both receives AND transmits (bidirectional node)
The MCU is a full bidirectional CAN node on **CAN1**, not a receive-only decoder.

- **Receive:** the CAN1_RX0 IRQ (`0x08007064`) reads FIFO0 into a **15-slot ×
  20-byte software RX ring** (write index in SRAM at `0x12c`).
- **Transmit:** there is a complete `CAN_Transmit` routine at **`0x08004cd2`**
  (the standard STM32 StdPeriph implementation). It:
  1. finds a free TX mailbox by testing the TSR **TME0/1/2** bits
     (`[base+0x08]` & `0x04/08/10000000`);
  2. loads the mailbox at `base + 0x180 + mailbox*0x10` — identifier
     (`StdId<<21` or `ExtId<<3`) + IDE/RTR into `TIxR`, `DLC` into `TDTxR (+4)`,
     data[0..3] into `TDLxR (+8)`, data[4..7] into `TDHxR (+0xc)`;
  3. sets the **TXRQ** bit (`orr …,#1` at `0x08004de6`) to send the frame.
- **It is actively used:** the caller at `0x08008080` drains a **15-slot × 20-byte
  software TX ring** (index in SRAM at `0x12d`) and transmits each record via the
  **CAN1** base (`0x40006400`).

TX message record layout (20 bytes): `+0x00` StdId(u16), `+0x04` ExtId(u32),
`+0x08` IDE, `+0x09` RTR, `+0x0a` DLC, `+0x0b…0x12` data[0..7].

Implication: because this is the `DCn32-VOLVO` build (§3.3), the frames it *sends*
are generated from a **Volvo** message profile. A Volvo-profiled node actively
transmitting onto a Toyota bus is a stronger concern than a passive decoder
mis-reading IDs — it can put unexpected frames on the Prado's bus.

### 3.2 Bluetooth AT commands (outbound to the Feasycom module)
These strings are **sent by the MCU to the BT chip**, not the STM32's own interface:
```
AT+NAME=DC_AUDIO_BT5.0,1   AT+PIN=0000     AT+LINKCFG=0
AT+LEDCFG=3                AT+AUDROUTE=1/2 AT+UPGRADE
```
Note `AT+UPGRADE` upgrades the **Feasycom BT module**, not the STM32 — unrelated
to MCU reflashing. `AT+PIN=0000` is the weak default legacy pairing PIN.

### 3.3 Version string
```
DCn32-VOLVO-V2.10-20240909
```
"DCn32" is the generic CAN-decoder product line; the compiled-in vehicle profile
is **VOLVO**, dated 2024-09-09. For a Toyota Prado this is the standout concern —
SWC key codes, reverse/ACC-IGN triggers and illumination decoding are
vehicle-specific. A Volvo profile will not correctly decode the Toyota bus
(Prado SWC is CAN ID `0x3C4` at 500 kbit/s per [../docs/1.2_CANBUS.md](../../docs/1.2_CANBUS.md)),
and — since the MCU also **transmits** on CAN1 (§3.1b) — it may put Volvo-profile
frames onto the Toyota bus. **Verify this is the intended image before flashing it
to a Prado.**

### 3.4 The application cannot program flash
Searched the whole image for the STM32 flash-unlock keys `0x45670123` /
`0xCDEF89AB` and the option-byte keys — **none are present.** The FLASH
peripheral base (0x40022000) is referenced only 3× (wait-states / flash-size
reads). All erase/program logic lives in the resident bootloader, not here.

---

## 4. Two-stage architecture (why this matters)

```
0x08000000  ┌──────────────────────────────┐
            │ Resident bootloader (16 KB)  │  ← YMODEM receiver, does the actual
            │  - NOT in this repo          │    flash erase/program
            │  - never leaves the chip     │
0x08004000  ├──────────────────────────────┤
            │ Application = can_app.bin    │  ← this file (CAN/keys/BT, no flash access)
0x0800BCFC  └──────────────────────────────┘
            (…up to 0x08020000 = 128 KB)
```

---

## 5. How the MCU update file is actually flashed

Reconstructed from `libMcuCenter.so` (SoC side, runs under MsnCoreApp on the
ARK1668). **The SoC never writes STM32 flash directly.** It acts as a **YMODEM
sender**; the resident bootloader inside the STM32 (§4) is the YMODEM *receiver*
and is the code that erases and programs the flash.

### 5.0 Trigger — `auto_upgrade.txt` (the "magic file")
The MCU update is **not** part of the SD `UpConfig`/`update` NAND-flash flow. It is
driven at runtime by `libMcuCenter.so` and triggered by inserting removable media:

1. Insert a USB stick / SD card. `DiskDeviceWatcher` mounts it under
   `/media/udisk/` (USB) or `/media/sdisk/` (SD) and fires `onDiskStatusChange`.
2. The P300/P307 adapter (McuType=6, this unit) scans the media for the trigger
   file **`auto_upgrade.txt`** and the payload **`can_app.bin`**
   (log strings: `find update file:` → `auto_upgrade.txt` → `onStartUpdateMCU()`;
   absence logs `not found update file!`).
3. Files are staged to `/tmp/mcuupdate/`, then `can_app.bin` is streamed to the
   MCU over `/dev/ttyHS0` by YMODEM (§5.2).

So the **magic trigger filename is `auto_upgrade.txt`**, accompanied by
`can_app.bin`. Drop both at the root of a FAT USB stick, insert it, and the unit
auto-flashes the MCU. (The generic base-class default name is `McuAppUpdate.img`;
the P300/P307 override this unit uses looks for `can_app.bin` + `auto_upgrade.txt`.)

**Location = partition ROOT (proven by disassembly).** In
`MCUAdapter_BoxP300::onDiskStatusChange` the check is built as:
```
r1 = "auto_upgrade.txt"
bl QString::fromAscii            ; QString("auto_upgrade.txt")
r0 = <mounted disk path>         ; a DiskDeviceWatcher mount, e.g. /media/udisk/
bl QString::append               ; path = <mountpoint> + "auto_upgrade.txt"
bl QFileInfo(path)::exists()     ; existence test
```
The mount path is appended **directly** with `auto_upgrade.txt` — there is **no
subfolder literal anywhere in the P300 override**. So the unit looks for
`<mountpoint>/auto_upgrade.txt` (e.g. `/media/udisk/auto_upgrade.txt`), i.e. the
**root of the inserted USB/SD partition**. (For contrast, the generic base class
`MCUAdapter_BoxP200::checkMCUUpdateFile` *does* use a `mcuupdate4/` subfolder with
`mcu_update.bin`, but the P300 adapter this unit uses never calls that path.)

### 5.1 Update file names seen in `libMcuCenter.so`
| Name | Meaning |
|---|---|
| `auto_upgrade.txt` | **Trigger flag** for the P300/P307 adapter (this unit). |
| `can_app.bin` | Firmware payload for the P300/P307 adapter (written to 0x08004000). |
| `McuAppUpdate.img` | Generic base-class default application-image name. |
| `McuSubUpdate.img` | Secondary / sub-processor image (optional). |
| `msnmcu_update.bin` / `mcu_update.bin` | Other adapters' payload names. |
| `mcuupdate_hud.bin` | HUD-variant image. |
| dir `mcuupdate4/`, `/tmp/mcuupdate/` | Search / staging directories. |

Config keys (from `MsnProductInfo.ini` / `FactoryConfig.ini`):
`MCUPortName="/dev/ttyHS0"`, `MCUBaudSpeed`, `MCUUpdateName`, `McuType=6`,
`CanType=0`, `DisableSetMCUType=1`.

### 5.2 Flow (symbols/log strings from `libMcuCenter.so`)
1. Update image placed in the firmware/USB dir (`mcuupdate4/`);
   `UpdateDialog::copyUpdateFileToTempDir` copies it to `/tmp/`. Name from `MCUUpdateName`.
2. **Trigger:** `onStartUpdateMCU()` (user action / disk insert / firmware package).
   `tryOpenUpdateFile` → `checkMCUUpdateFile` validate it.
3. `Open MCU Serial Port` (ttyHS0 at `MCUBaudSpeed`).
   `readyToUpdateMCU` / `sendReadyPackage` / `onSendUpdateReadyTimer` send an
   in-band command over the normal MCU protocol that tells the running app to
   **hand off / reset into its resident bootloader** and listen for YMODEM.
4. `sendUpdateFileInfo()` → YMODEM **block 0** (filename + size). Log: `Start send update packaget 0`.
5. `sendUpdateFileData(i)` / `sendYModemDatas(...)` stream 128/1024-byte YMODEM
   blocks, each with **`CRC16_YMODEM`**, waiting for ACK; on NAK it resends
   `gLastUpdateByteArray` (log: `ReSend Last Update Datas:`).
   `UpdateDialog::updatePercent` / `setUpdateFailed` drive the progress UI.
   `gUpdateMasks` / `C UpdateMask:` select which images (App vs Sub) to send.
6. STM32 bootloader erases the app region and programs each block into
   0x08004000+. EOT → log `End Update Mcu!`.
7. `RequestRebootSystem` / the MCU resets into the new application.

### 5.3 Consequences
- **Write-only push.** YMODEM upload provides **no read-back** path. There is no
  documented way to read/dump MCU flash back over `/dev/ttyHS0`.
- **Integrity only, no authenticity.** Transfer is protected by CRC16-YMODEM
  (corruption detection) — there is **no signature / authentication** of the
  image (matches [../docs/3.2_SECURITY_REVIEW.md](../../docs/3.2_SECURITY_REVIEW.md): "no firmware
  update signature/checksum verification"). Anyone with root on the SoC can drop
  a `McuAppUpdate.img` and trigger a flash of arbitrary MCU firmware.
- The resident bootloader is required for this to work and is **not recoverable
  from the SoC** — it never transits `ttyHS0`.

---

## 6. SoC-side CAN — does the head unit command the MCU to transmit?

The MCU firmware *can* transmit on CAN (§3.1b). A separate question is whether the
**SoC software** (`libMcuCenter.so`) ever tells it to. For the Prado as
configured: **no.**

- **The Prado's MCU adapter has no CAN methods.** `McuType=6` selects
  `MCUAdapter_BoxP300`, whose full method set is the generic MCU protocol
  (`makeMCUProtocol`, `onRecvMcuProtocol`, `syncSettingDataToMcu`,
  `onModeAppChanged`, settings-UI getters, update path). There is **no
  `writeCanBusData` / `makeCanBusProtocol` / `sendCan*`** — it only exchanges
  pre-decoded key events, mode/phone state, settings and firmware updates.
- **`CanType=0` loads no SoC-side CAN adapter** (0 = none; same convention as
  `SoundType=0`). So no `libCanBus` adapter is instantiated either.

**Where the capability *does* live.** Only `McuAdapter_BoxP230` (Honda XBS) in
`libMcuCenter.so` can push CAN through the MCU. Its path (disassembled):
```
writeCanBusData(type, data*, len)
  → makeCanBusProtocol(...)      builds a QByteArray frame [header][type][subtype][payload][checksum]
  → ProtocolUtils::writeDatas()  writes that frame to the MCU serial port (ttyHS0)
```
i.e. "send a CAN command to the MCU" = write a specially-typed serial frame over
`ttyHS0`, which the MCU then transmits on the vehicle bus. This is wired up only
in BoxP230, not BoxP300.

### 6.1 `libCanBus.so` — external CAN-box adapters (selected by `CanType`)
Separately, `libCanBus.so` holds per-brand adapters for **external CAN-decoder
boxes** (not the STM32 MCU — these talk to a dedicated CAN module over their own
serial link). `CanBusAdapter::getAdapterInstance(CanBusType)` is a jump table
(`sub r3,type,#1; cmp r3,#0xe; addls pc,…`), so **CanType 0 = none, 1–15 valid**:

| CanType | Adapter class | Vendor / protocol | Car brand |
|:---:|---|---|---|
| **0** | — | *(none — MCU handles CAN itself; the Prado's setting)* | — |
| 1 | `CanBus_LiHang_JMCE200N` | LiHang JMCE200N | generic |
| 2 | `CanBus_Huida_ZD` | Huida ZD | generic |
| 3 | `CanBus_Raise_Volkswagen` | Raise | Volkswagen |
| 4 | `CanBus_XinHang` | XinHang | generic |
| 5 | `CanBus_XBS_Mazda` | XBS | Mazda |
| 6 | `CanBus_XinRi` | XinRi | generic |
| 7 | `CanBus_Raise_Honda` | Raise | Honda |
| 8 | `CanBus_Raise_Nissan` | Raise | Nissan |
| **9** | `CanBus_Raise_Toyota` | **Raise** | **Toyota** |
| 10 | `CanBus_Raise_GM` | Raise | GM |
| 11 | `CanBus_Raise_Haval` | Raise | Haval |
| 12 | `CanBus_Raise_GAC` | Raise | GAC |
| 13 | `CanBus_Raise_Venucia` | Raise | Venucia |
| 14 | `CanBus_Raise_Renault` | Raise | Renault |
| 15 | `CanBus_Raise_Jeep` | Raise | Jeep |

`CanBus_DaoJun_Honda` is compiled in but has **no `CanType` slot** in this factory
(selected by some other path / dead code).

**Takeaways for the Prado:** the STM32 MCU is the sole CAN endpoint — it decodes
Toyota CAN and forwards key events, and the SoC (`BoxP300` / `CanType=0`) never
commands it to transmit. **CanType 9 (`CanBus_Raise_Toyota`)** is the SoC-side
Toyota option, but it targets an *external Raise CAN box*, not this MCU, and is
switched off here. Enabling SoC-driven CAN would mean either swapping the MCU
adapter to `BoxP230` or fitting a Raise box and setting `CanType=9`.

---

## 7. Dumping the existing MCU flash

There is **no read-back in the normal software stack**: the running app has no
memory-read command and cannot touch flash (§3.4), and the SoC updater is a
write-only YMODEM push (§5.3). Options, best-first:

**Route 1 — You may already have it (do first).** The factory image is likely a
file already on the device (`mcuupdate4/McuAppUpdate.img`, `msnmcu_update.bin`,
etc.). This `can_app.bin` almost certainly came from such a file. Grab it and
compare its version string against `DCn32-VOLVO-…`. Caveat: it is the *update*
image the vendor shipped, which may differ from what is physically on the chip.

**Route 2 — STM32 factory ROM bootloader (AN3155) over UART** — the only
*documented* serial read. Blockers, in order:
- **BOOT0 = 1 at reset** (hardware pin). If the board doesn't route BOOT0 to a
  SoC-controllable GPIO (it usually doesn't), you cannot enter it from software
  over `ttyHS0`; you need physical access to strap BOOT0 + reset.
- **`ttyHS0` must land on a bootloader-capable UART** (F105 ROM: USART1/USART2 or
  CAN/DFU per AN2606). The app link looks like USART2 (0x40004400), which the ROM
  supports — confirm the physical pin mapping.
- **Read-out protection (RDP).** If option-byte RDP ≥ level 1, Read Memory
  returns NACK and any bypass triggers mass-erase. RDP state is not in this image;
  check it first. If set, serial dump is impossible without wiping.
- If all clear: `stm32flash -r dump.bin /dev/ttyHS0` from the SoC (cross-compile
  `stm32flash` for the ARK's ARMv7 Linux, or script AN3155 at even parity).

**Route 3 — SWD (most reliable; not via `ttyHS0`).** ST-Link/J-Link on
SWDIO/SWCLK, then `st-flash read dump.bin 0x08000000 0x20000` reads the full
128 KB — **including the 16 KB resident bootloader this file lacks**, which is
what you actually want for a complete reconstruction. Blocked only by RDP.

**Recommended:** (1) harvest `mcuupdate4/` off the device and diff versions;
(2) live-capture `cat /dev/ttyHS0 | hexdump -C` while pressing SWC keys to learn
framing/baud and confirm which STM32 UART it is; (3) before any serial attempt,
determine RDP + BOOT0 accessibility; otherwise use SWD for a guaranteed complete dump.

---

## 8. Summary of notable findings
- Structurally valid, uncorrupted STM32F105 CAN-gateway app with watchdog enabled.
- **Bidirectional CAN node on CAN1** — receives (CAN1_RX0 IRQ) *and* transmits
  (`CAN_Transmit` at `0x08004cd2`, driven by a 15-slot TX ring); see §3.1b.
- **Volvo** vehicle profile (`DCn32-VOLVO-V2.10-20240909`) — likely wrong for a Prado; and because it *transmits*, it may put Volvo-profile frames on the Toyota bus. Verify before flashing.
- Application has **no flash-programming code**; a separate resident bootloader (0x08000000–0x08004000, not in repo) does updates.
- MCU updates are a **YMODEM push** (`McuAppUpdate.img`) from the SoC over `/dev/ttyHS0` — CRC-protected but **unauthenticated**.
- **No serial read-back** exists; dumping the live chip needs the ROM bootloader (BOOT0 + no RDP) or SWD.
- **SoC does not drive CAN** — `BoxP300` (`McuType=6`) has no CAN methods and `CanType=0` loads no CAN adapter; the MCU is the sole CAN endpoint. The Toyota SoC-side option is `CanType=9` (`CanBus_Raise_Toyota`, an external Raise box), unused here. See §6.
- BT module uses default PIN `0000`.
