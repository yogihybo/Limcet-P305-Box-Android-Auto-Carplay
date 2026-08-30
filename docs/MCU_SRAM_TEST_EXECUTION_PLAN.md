# Running the Clean-Room MCU Firmware From SRAM via SWD — No Flash Write, No Bootloader

**Status**: plan only, not yet executed (matches the standing convention
of `docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md` -- documented in full before
being tried on real hardware).

Lets `hardware/MCU/source/` (this project's clean-room reimplementation)
run on the real STM32F105RBT6, for real, interactive testing -- CAN1
traffic, UART4/5 probing, GPIO behavior -- **without ever writing a
single byte to flash**, and therefore without any of the real risk this
project already flagged for the `CMD 0xE1`/YMODEM reflash path (see
`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s `CMD 0xE1` section: no
confirmed dump of this unit's real firmware exists to restore if a
flash write goes wrong).

---

## Why this works, and why it's a genuinely different risk class than reflashing

Cortex-M3 (this chip's core) has no restriction against executing code
from SRAM -- `SCB->VTOR` can point anywhere in addressable memory, and
instruction fetches over the AHB bus work identically whether the
backing memory is flash or SRAM. The mechanism:

1. Connect SWD, `reset halt` (forces the same reset every other SWD
   plan in this project already causes -- expected, not new).
2. Freeze the watchdog via `DBGMCU_CR`'s `DBG_IWDG_STOP`/`DBG_WWDG_STOP`
   bits -- **already a proven technique in this project's own
   `tools/can-sniffer/`**, not new ground. Without this, the IWDG
   (confirmed enabled in the real firmware) would keep counting down
   while the CPU sits halted for inspection, and fire the moment
   execution resumes.
3. Write a specially-built test image directly into SRAM (`0x20000000`+)
   via OpenOCD's binary-load commands.
4. Set `SCB->VTOR` to point at the SRAM-resident vector table, load
   `SP`/`PC` from that table's first two words, resume.

**Why this doesn't touch RDP1 at all.** The confirmed mechanism from
this project's own live-hardware finding (`docs/MCU_FIRMWARE_VERIFIED_
FINDINGS.md`'s "CRITICAL SAFETY FINDING") is that RDP Level 1 BusFaults
the CPU's *next read of protected flash content* while a debugger is
attached. A test image that lives entirely in SRAM, whose code never
branches into or dereferences a flash address, never touches the
region RDP1 protects -- there's nothing for that mechanism to fire on.

**Why this doesn't risk the real firmware at all.** No flash erase,
unlock, or write instruction executes anywhere in this whole procedure
-- confirmed by direct inspection of this project's own clean-room
source (`hardware/MCU/source/src/*.c`): zero references to the
`FLASH->` peripheral register block anywhere. The real firmware
currently on the chip is never read from, written to, or erased.

## The persistence question, answered up front (asked and settled before writing this plan)

**SRAM's raw content survives a warm reset** (watchdog, `NRST` pin, or
software `AIRCR` reset all just restart CPU execution -- they don't
electrically clear SRAM cells; only a power-on/brown-out reset does).
This project already relies on that exact fact for the `CMD 0xE1`
magic-cookie mechanism.

**But `SCB->VTOR` itself does NOT survive a reset** -- it resets to its
hardware default (pointing at flash/system-memory per `BOOT0`/`BOOT1`)
on every reset, warm or cold. So test code injected this way does not
"keep running" or auto-resume after any reset: the chip boots the real,
flash-resident firmware exactly as if this session never happened, and
that firmware's own `Reset_Handler` will overwrite some or all of SRAM
via its own `.bss`/`.data` init in the process. **This is a live,
re-inject-each-time methodology, not a persistent flash replacement** --
and that's a real safety property, not a limitation: ending the SWD
session (reset or detach) cleanly and completely hands control back to
the real firmware, with nothing left behind to clean up.

---

## Real source changes needed (not yet made)

Two things in `hardware/MCU/source/` assume a flash-resident build and
need a genuine SRAM-target variant, not just a different `-T` flag:

### 1. New linker script, `stm32f105_sram_test.ld`

The existing `stm32f105_app.ld` splits `.data`'s load address (flash)
from its run address (RAM) via `> RAM AT > FLASH` -- meaningless for a
build that's never in flash at all. The SRAM variant collapses
everything into one region, load address == run address, no copy
needed:

```
ENTRY(Reset_Handler)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* same as today -- top of SRAM */

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > RAM
    .text       : { *(.text) *(.text*) *(.rodata) *(.rodata*) } > RAM
    .data       : { _sdata = .; *(.data) *(.data*) _edata = .; } > RAM
    .bss        : { _sbss = .; *(.bss) *(.bss*) *(COMMON) _ebss = .; } > RAM
    /DISCARD/   : { *(.note*) *(.comment) *(.ARM.exidx*) *(.ARM.extab*) }
}
```

Real, checked headroom: the current clean-room build is genuinely tiny
(`.text`=3540B, `.data`=544B, `.bss`=672B -- 4756 bytes total, per
`arm-linux-gnueabihf-size`) against 64KB of SRAM. Even a substantially
larger test build has enormous room; no practical size risk.

### 2. `Reset_Handler`'s hardcoded `VTOR` write

`hardware/MCU/source/src/startup_stm32f105.c`'s `Reset_Handler()`
currently does `SCB->VTOR = FLASH_APP_BASE;` unconditionally -- correct
for the real flash build, wrong for SRAM (it would immediately redirect
`VTOR` back to `0x08004000`, where nothing valid exists in this
scenario, faulting right after the SWD-injected `VTOR` was set
correctly). Needs a build-time switch:

```c
#ifdef SRAM_TEST_BUILD
#define VECTOR_TABLE_BASE  0x20000000UL
#else
#define VECTOR_TABLE_BASE  FLASH_APP_BASE
#endif
    SCB->VTOR = VECTOR_TABLE_BASE;
```

The `.data` copy loop (`_sidata` -> `_sdata`) can stay exactly as-is --
with the new linker script's load address equal to its run address,
`_sidata == _sdata` and the loop becomes a harmless self-copy no-op,
not worth special-casing out.

### 3. New Makefile target

```makefile
sram-test: CFLAGS += -DSRAM_TEST_BUILD
sram-test: LDFLAGS = $(MCU_FLAGS) -Tstm32f105_sram_test.ld -nostdlib -static -Wl,--build-id=none -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(TARGET)_sram.map
sram-test: $(BUILD_DIR)/$(TARGET)_sram.bin
```
(mechanical -- mirrors the existing `all` target's object/link/objcopy
chain with the substituted linker script and define; not written out
in full here, real implementation work for whenever this plan is
executed.)

---

## Procedure

### Phase 1 — Connect and prepare (same opening as every other SWD plan here)

```
reset halt
reg pc
```
Sanity-check the halted PC lands somewhere sane before proceeding.

### Phase 2 — Freeze the watchdog

`DBGMCU_CR` is at `0xE0042004` on this chip (confirmed address, same
one `tools/can-sniffer/` already uses successfully):
```
mdw 0xE0042004
```
`NEW = OLD | (1<<8) | (1<<9)`  (`DBG_IWDG_STOP` | `DBG_WWDG_STOP`)
```
mww 0xE0042004 <NEW>
```

### Phase 3 — Load the test image into SRAM

```
load_image build/can_app_sram.bin 0x20000000 bin
```
(OpenOCD's `load_image` writes the raw binary directly via the debug
port -- no bootloader, no YMODEM, no flash controller involved at all.)

### Phase 4 — Point the CPU at it

```
mdw 0x20000000    ; confirm this reads back the real initial SP value
mdw 0x20000004    ; confirm this reads back the real Reset_Handler address
mww 0xE000ED08 0x20000000    ; SCB->VTOR = 0x20000000
reg sp <value from 0x20000000>
reg pc <value from 0x20000004>
```

### Phase 5 — Run it

```
resume
```

From here, the chip is executing the clean-room test build entirely
from SRAM. Real hardware interaction (CAN1, UART4/5, GPIO) works
normally -- the peripherals themselves don't care where the code
driving them lives. Use `tools/can-sniffer/`-style `mdw` polling, or
attach a UART probe, to observe real behavior.

### Phase 6 — End the session cleanly

```
reset run
```
or simply disconnect. Per the persistence discussion above, this
returns the chip to booting its real, flash-resident firmware with
nothing left over from the test -- no cleanup step needed, by
construction.

---

## Real caveats, stated plainly

- **Connecting SWD holds the whole head unit in reset** (the
  already-confirmed `GPIOB14` finding) -- the ArkMicro SoC and screen
  go dark for the whole test, same tradeoff `tools/can-sniffer/`
  already has. Not a new cost, just inherited.
- **The physical CAN transceiver's power/enable state is unconfirmed**
  independent of anything this test controls -- same open caveat
  `docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md` already flags; if `CAN1->RF0R`
  never increments despite real bus traffic, this is the likely
  explanation, not a sequencing mistake in this plan.
- **This is genuinely a live/interactive test only** -- see the
  persistence section above. Don't expect a test build to still be
  running after a power cycle or unattended reset; that's not a bug in
  this plan, it's the actual hardware behavior.
- **The Makefile target and linker script above are specified but not
  yet created** -- real, small, mechanical implementation work,
  deliberately left for when this plan is actually executed rather than
  spinning up build infra ahead of a documented go-ahead.

## What this is genuinely good for

Iterating on hypotheses this session's static analysis reached a real
limit on -- for example, the UART4/5 investigation's open question
(`docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md`'s "what information transits
UART4/5" section): a modified test build with extra instrumentation, or
one that directly exercises the same `0x20`/`0x32`/`0x50`/`0xD3`/`0xD6`
message types against real hardware, could observe real behavior no
amount of further disassembly can settle on its own -- with zero risk
to whatever's actually flashed on the device.

## Critical files
- `hardware/MCU/source/stm32f105_app.ld` -- reference for the new
  SRAM-variant script above, not modified
- `hardware/MCU/source/src/startup_stm32f105.c` -- needs the
  `SRAM_TEST_BUILD` conditional described above
- `hardware/MCU/source/Makefile` -- needs the new `sram-test` target
- `tools/can-sniffer/` -- reference implementation for the
  OpenOCD-Tcl-RPC connection pattern and the `DBGMCU_CR` freeze
  sequence this plan reuses verbatim
- `docs/MCU_CAN_BUS_SWD_SNIFFING_PLAN.md` -- sibling plan, same
  underlying SWD capability, register-level rather than full-program
  execution
