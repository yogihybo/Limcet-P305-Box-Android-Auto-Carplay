# Ark1680 Ts Reverse Engineering

**Status:** Reference
**Last Updated:** 2026-07-16

## Overview

**Why:** stock firmware on this unit selects `ark1680_ts.ko` at boot, not
GT911 (see `docs/historical/boot_experiment_log.md` → "ROOT CAUSE FOUND"). No source
exists anywhere in this repo or the vendor kernel trees under
`~/Downloads/` — only the compiled 3.4.0 `.ko` in the firmware dump and
its board-registration code in the stock `vmlinux`. Recovered by static
disassembly (`arm-linux-gnueabihf-objdump`/`readelf`) of:

- `Prado firmware reconstructed/mtd6_rootfs/rootfs/lib/modules/3.4.0/kernel/drivers/input/touchscreen/ark1680_ts.ko` (not stripped — full symbol table)
- `Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf` (board-file device/resource registration)

## Hardware resources (from `ark1680_add_device_ts` @ `0x8059f0c4` in `vmlinux.elf`)

The board file registers a `struct platform_device` at `0x805c7918`
(name `"ark1680-ts"`) with 2 resources, found at its `resource` pointer
`0x805c8150`:

| Resource | Value |
|---|---|
| MEM | `0xe4500000` – `0xe4500040` (span ~0x41 bytes — small register block) |
| IRQ | `4` (raw VIC line number, same numbering space as `interrupt-parent = <&vicl>` elsewhere in the 4.19 DTS) |

The driver also does a **second, hardcoded** `ioremap(0xe4900000, 0x200)`
in `ark1680_ts_probe` (not resource-based, not DT-derived — literal
constant in the code) for the SoC's shared "sregs"/pinctrl syscon block.
This is the **same physical block** already declared in the 4.19 DTS as
`pinctrl0`/`sregs@e4900000` (`ark1668.dtsi:69,349`, size `0x1000`) — so a
new driver can reference it via a phandle/syscon lookup instead of a raw
second `ioremap`, if written cleanly, or just replicate the raw
`ioremap` for a literal first-pass port.

## Register map — ADC/TSC block (`ark_adc_mmio_base`, phys `0xe4500000`)

Recovered from `Enable_ADC_Channel`, `ark1680_setup_tsc`, `SetDBCNT`,
`SetDETInter_new`, and the start of `ark1680_ts_interrupt`:

| Offset | Purpose |
|---|---|
| `+0x00` | Channel/mode enable — bits OR'd in: bit0 (base enable), bits 1–6 per-ADC-channel enable (`Enable_ADC_Channel(1..6)` sets bits 1,2,3,4,5,6 respectively), bits 8,9,10,11 also set during `ark1680_setup_tsc` (touch-specific modes) |
| `+0x08` | Per-channel config/threshold — `Enable_ADC_Channel` clears a different bitfield per channel (e.g. ch1 clears `0x8000`, ch2 clears `0x7000`, ch3 clears `0x7`, ch4 clears `0x38`, ch5 clears `0x1c0`, ch6 clears `0xe00` — looks like a packed 3-bit-per-channel field starting around bit 6); `ark1680_setup_tsc` also writes `7` here directly |
| `+0x0c` | Interrupt status/ack **and** general clear register — cleared to 0 for some channels in setup; in the IRQ handler, each cause bit (bit0..bit4+) is individually cleared here to ack that source |
| `+0x14` | Read after acking each IRQ cause bit — likely the per-channel ADC conversion result/data register |
| `+0x28` | Sample counter, compared against `512` in the IRQ handler; indexes a large (34816-byte) raw-sample ring buffer (`int_flag[]` in `.bss`) — looks like debug/history logging, not required for basic function |
| `+0x2c` | Debounce count (`SetDBCNT`) — stock sets `40000` (`0x9c40`) |
| `+0x30` | Detect interval (`SetDETInter_new`) — stock sets `130` (`0x82`) |

## Register map — syscon/pinctrl block (`ark_sys_mmio_base`, phys `0xe4900000`)

| Offset | Purpose |
|---|---|
| `+0x48` | OR bit `0x8` — clock/pin enable for the ADC/TSC block |
| `+0x50` | OR bit `0x800` (bit 11) — pad/pull config |
| `+0x64` | ADC clock divider (`DivADCCLK`) — clears bits `0xfffe`, then ORs in `(div << 1)`; stock calls `DivADCCLK(239)` from `ark1680_setup_tsc` |
| `+0x140` | Clear bit `0x400000` (bit 22) — pinmux clear |
| `+0x144` | Clear bit `0x4000` (bit 14) — pinmux clear |

## Driver flow (from `ark1680_ts_probe` + `ark1680_setup_tsc`)

1. `platform_get_resource(pdev, IORESOURCE_MEM, 0)` → request_region +
   `ioremap` → `ark_adc_mmio_base`.
2. `platform_get_irq(pdev, 0)` → IRQ 4.
3. Hardcoded `ioremap(0xe4900000, 0x200)` → `ark_sys_mmio_base`.
4. `ark1680_setup_tsc()`:
   - `DivADCCLK(239)` — set ADC clock divisor.
   - syscon `+0x48` OR `0x8`, `+0x50` OR `0x800`, `+0x140` clear
     `0x400000` — enable/pinmux sequence.
   - ADC base `+0x00` OR `0x1` then clear `0x7e` — base enable, mask
     off some mode bits.
   - ADC base `+0x08` = `7`, `+0x0c` = `0`.
   - `SetDBCNT(40000)`, `SetDETInter_new(130)`.
   - syscon `+0x144` clear `0x4000`.
   - ADC base `+0x00` OR `0x100`, OR `0x200`, OR `0x400`, OR `0x800`
     (four more mode/channel-enable bits).
   - `Enable_ADC_Channel(2)` — enables the touch X/Y sampling channel(s).
5. `request_threaded_irq` on IRQ 4 → `ark1680_ts_interrupt`.
6. IRQ handler reads ADC `+0x0c` (cause bits), for each set bit reads
   `+0x14` (result) and acks by clearing that bit in `+0x0c`; raw samples
   also logged into the large `int_flag[]` buffer keyed by a `+0x28`
   counter (debug/history — skip for a minimal port). State machine
   variables (`bFindStart`, `bFindStop`, `PrevX/PrevY`, `TmpX/TmpY`,
   `Tspsta`) drive touch-down/touch-up detection and feed `TSP_GetXY`
   (376 bytes, **not yet disassembled** — computes final X/Y from the
   `point_x[]`/`point_y[]` sample arrays, likely a 4-sample average/filter)
   before calling `input_event`/`input_report_abs`.

## `TSP_GetXY` — coordinate filter (decoded)

`int TSP_GetXY(int *out_x, int *out_y, int enable)`:

- `enable == 0`: resets the sample `count` to 0, returns immediately
  (used on touch-release to flush the filter state).
- Each call reads two **new raw ADC registers**, right-shifted by 1:
  - ADC base `+0x24` → raw X sample
  - ADC base `+0x28` → raw Y sample
- Pushes the pair into a 4-slot circular buffer (`point_x[4]`,
  `point_y[4]` in `.bss`), keyed by `count & 3`.
- Requires `count > 4` (i.e. at least 5 IRQs) before producing a result —
  returns "not valid yet" (`0`) until then.
- Once warmed up: copies the 4 buffered X samples and 4 Y samples into
  scratch arrays, **sorts each independently** (simple compare-swap
  network, not a joint sort — X and Y are filtered independently), then:
  - `*out_x = (X_sorted[1] + X_sorted[2] + 1) >> 1` (average of the two
    middle values — median-of-4)
  - `*out_y = (Y_sorted[1] + Y_sorted[2] + 1) >> 1`
  - stability check: `spread_x = X_sorted[2]-X_sorted[1]`,
    `spread_y = Y_sorted[2]-Y_sorted[1]`; returns **valid (1)** only if
    both spreads are `<= 80` (ADC units), else **unstable (0)**.

## Input event protocol (from the tail of `ark1680_ts_interrupt`)

Standard resistive-touch event sequence, confirming `EV_ABS`/`EV_KEY`
codes (matches what `tslib` expects, consistent with the stock
`QWS_MOUSE_PROTO=tslib:...` config selected for this driver):

**Touch down** (once `TSP_GetXY` returns valid/stable):
```
input_event(EV_ABS, ABS_X,        x)
input_event(EV_ABS, ABS_Y,        y)
input_event(EV_ABS, ABS_PRESSURE, 4095)
input_event(EV_KEY, BTN_TOUCH,    1)
input_event(EV_SYN, SYN_REPORT,   0)
```

**Touch up**:
```
input_event(EV_ABS, ABS_PRESSURE, 0)
input_event(EV_KEY, BTN_TOUCH,    0)
input_event(EV_SYN, SYN_REPORT,   0)
```

No `ABS_X`/`ABS_Y` re-sent on release (matches `ark-nop-ts.c`'s
`input_set_abs_params(ABS_X/Y/PRESSURE, 0, 4095, 5, 0)` range convention
— reuse that for the new driver).

## Remaining unknowns (acceptable for first bring-up)

- Exact per-channel bitfield packing at ADC `+0x08` (inferred from
  differing clear-masks per channel, not fully decoded bit-by-bit) — the
  port below replicates the literal masks rather than deriving a formula.
- Cross-check IRQ `4` against `ark1668.dtsi`'s `&vicl` numbering to
  confirm no collision with another peripheral's raw VIC line in the
  4.19 DTS.
- The large `int_flag[]` debug/history buffer and `+0x28` sample counter
  in the IRQ handler are logging-only — omitted from the port.

## Port status

Implemented and **compiles clean** (`make drivers/input/touchscreen/ark1680_ts.o`,
`W=1`, zero warnings) against the real 4.19 build tree:

- `Limcet Hardware/ark1680_ts.c` — canonical source, mirrored into
  `~/Downloads/linux-arkmicro/linux/drivers/input/touchscreen/ark1680_ts.c`
- Wired into that tree's `drivers/input/touchscreen/Kconfig` (new
  `CONFIG_TOUCHSCREEN_ARK1680`, modeled on the existing `ARK_NOP` stub)
  and `Makefile` (`obj-$(CONFIG_TOUCHSCREEN_ARK1680) += ark1680_ts.o`)
- `CONFIG_TOUCHSCREEN_ARK1680=y` set in both the external tree's
  `.config` and the canonical `Limcet Hardware/kernel_dot_config` (built
  in, not a module — no rootfs/rcS insmod wiring needed)
- `tsc@e4500000` DTS node added to
  `Limcet Hardware/ark1668-limcet-prado.dts`; `make dtbs` builds clean
  and the node decompiles back correctly

**Debugging support built in:**
- Probe logs every milestone at `dev_info` level unconditionally: the MEM
  resource claimed (`%pR`), the IRQ number, the syscon block mapping, a
  full register dump right after `ark1680_setup_tsc()` (ADC enable/config/
  irq-status regs + syscon clken/padcfg/clkdiv regs), and a final
  "registered" line. All error paths (`dev_err`) include the actual
  error code, not just a bare message.
- A `debug` module parameter (`insmod ark1680_ts.ko debug=1`, or
  `echo 1 > /sys/module/ark1680_ts/parameters/debug` at runtime) turns on
  per-IRQ tracing: raw ADC status word, each raw `(x,y)` sample and
  warm-up progress, the filtered coordinate + spread + stable/unstable
  verdict, and DOWN/UP transitions. Off by default since it fires every
  touch IRQ; `dmesg | grep ark1680_ts` alone is enough for basic bring-up
  without it.

**Hardware-tested (2026-07-11, `docs/new kernel bootlog new uboot v7.txt`):**

```
ark1680_ts e4500000.tsc: probe: ADC/TSC MEM resource [mem 0xe4500000-0xe450003f]
ark1680_ts e4500000.tsc: probe: irq=20
ark1680_ts e4500000.tsc: probe: mapped syscon/pinmux block at phys 0xe4900000, size 0x200
ark1680_ts e4500000.tsc: probe: post-setup regs: ADC[en=0x00000f04 cfg=0x00000007 irq=0x00000000] SYS[clken=0xffffffff padcfg=0xffffffff clkdiv=0x002101de]
input: ark1680-ts as /devices/platform/e4500000.tsc/input/input0
ark1680_ts e4500000.tsc: ARK1680 resistive touchscreen registered, irq=20, debug=0
```

**Probe succeeds cleanly, no crash, input device registers.** `irq=20` is
the expected Linux virq for hwirq 4 on the `vicl` domain (not a bug).
`ADC[en=0x00000f04]` = bits `{2,8,9,10,11}` — matches `Enable_ADC_Channel(2)`
+ the setup-tail mode bits exactly; bit 0 (OR'd first) reads back 0,
revealing it's a **self-clearing strobe bit** on real silicon, not a
persistent enable — new information the static disassembly couldn't give us.

**Anomaly:** `SYS[clken=0xffffffff padcfg=0xffffffff]` — all-ones,
a classic bus-fault/decode-error readback, not real register content.
`clkdiv=0x002101de` in the same block reads back sane (`239<<1=0x1de`
visible in the low bits, matching `DivADCCLK(239)`), so the syscon
mapping itself works — it's specifically the `+0x48` (`ARK_SYS_CLKEN`)
and `+0x50` (`ARK_SYS_PADCFG`) writes that break subsequent reads to
that sub-block. Since `pinctrl0` (same `0xe4900000` block, no `clocks`
property at all) works fine throughout the rest of boot (LCD/camera/WiFi
all up), this isn't a clock-gating issue — it's most likely a **wrong
bit or inverted polarity** in the `CLKEN`/`PADCFG` writes (e.g. the bit
we assumed was "enable" may actually assert reset/power-down on that
sub-block, knocking it off the bus for further reads).

**Update (v8, per-write trace, `docs/new kernel bootlog new uboot v8.txt`):**
added 10 trace points bracketing every write in `ark1680_setup_tsc()`.
Result: `clken`/`padcfg` read `0xffffffff` **even in the `initial` trace,
before the driver writes anything at all**:
```
setup[initial]: ... SYS[clken=0xffffffff padcfg=0xffffffff clkdiv=0x00210016 pmux0=0xc5640003 pmux1=0x000623b8]
```
This overturns the "our write broke it" theory from the v7 analysis
above — it never worked, cold-boot or not. Meanwhile `clkdiv`/`pmux0`/
`pmux1` at the same 0xe4900000 block all read sane values *and* change
correctly in response to their respective writes (`clkdiv`
`0x00210016→0x002101de` after `DivADCCLK`; `pmux0` bit 22 clears exactly
as coded). So this isn't a block-wide bus fault or missing clock —
**it's specific to just those two offsets**.

Best explanation: the stock disassembly **never reads back**
`CLKEN`/`PADCFG` after writing (`Enable_ADC_Channel`/`ark1680_setup_tsc`
only write there) — only this port's added debug tracing reads them.
They're most likely **write-only/strobe-style registers** on this SoC
that simply don't return meaningful data on read — a real, if unusual,
hardware pattern, not a driver bug. Treat their `0xffffffff` readback as
expected/harmless going forward; it doesn't indicate the writes failed
to take effect (`pmux0` proves writes to this block do land correctly).

**Update (`docs/ark ts scan v1.txt`) — raw ADC never moves:** ran
`ark-ts-test regs` 10 times, including while touching the panel and
after `start_msn` (segfaults again — the pre-existing, unrelated
`MsnCoreApp` bug, see `HANDOFF_bootlog_v6_review.md` Blocker 2). Result,
every single time:
```
[+0x00c] irq_status   = 0x00000000
[+0x014] adc_result   = 0x00000000
[+0x024] raw_x        = 0x00000000
[+0x028] raw_y        = 0x00000000
```
**Pinned at zero across all 10 runs, `irq_status` never transiently
non-zero either** — the ADC/TSC hardware never signals a touch-detect
event at all, not even once. (Side-note: `clken`/`padcfg` read
`0xfffdf9ad`/`0xffff19ff` here, differing from the driver's own
`0xffffffff` probe-time trace — expected, since that's a *shared* syscon
block other subsystems also write to over the course of boot; not
meaningful for touch and not a driver issue.)

This is the strongest signal so far that the problem is no longer in
software. `ch_enable`/`ch_config` show the driver's writes took
(`0x00000f04`/`0x00000007`, matching `docs/ARK1680_TS_REVERSE_ENGINEERING.md`'s
recorded init sequence), so the ADC block accepted configuration — it
just never produces a conversion result, in 10 attempts including live
touch and app-launch attempts.

**Open question before doing more software work:** was the panel firmly
touched during each of these runs, and is the debounce/detect logic
(`DETINTER`/`DBCNT`, or an undiscovered "arm/start" trigger beyond the
one-shot `bit0` strobe already identified) actually enough to make this
block sample continuously, or does it need re-triggering per sample
(unlike a free-running ADC)? Two possible next steps, in order of cost:

1. **Cheap, do first:** `echo 1 > /sys/module/ark1680_ts/parameters/debug`,
   `dmesg -w`, and firmly drag a finger across the panel while watching
   for *any* `irq status=` line — confirms definitively whether the IRQ
   ever fires, independent of the polling-based `ark-ts-test` snapshots
   possibly missing a narrow window.
2. **If step 1 shows nothing:** this becomes a hardware question, same
   conclusion the GT911 investigation reached independently — physically
   check the touch panel FPC connector is seated
   (`Limcet Hardware/board_photo_*.jpg`), and consider whether this
   specific unit's touch panel is disconnected/absent/damaged rather than
   assuming more register bits remain to be found. Two independent touch
   technologies (GT911 I²C and now the ARK1680 resistive ADC) both
   showing zero hardware response is a pattern, not a coincidence — at
   this point "the panel isn't wired/seated/present" is the leading
   hypothesis, not a remaining software bug.

### Ruled out: touch via MCU or a serial touch controller

Before concluding "physical issue," two alternative subsystems were
considered and ruled out — both are dead ends, not remaining leads:

- **MCU (`/dev/ttyHS0`) forwarding touch** — `Limcet Hardware/BOARD_ANALYSIS.md`
  previously claimed this, "confirmed via MCU Monitor." Retracted: direct
  on-screen observation showed the MCU Monitor only displays CAN-bus
  activity, and a live byte capture of `/dev/ttyHS0` showed **zero
  traffic at all** (not even the idle status frames expected regardless
  of touch).
- **A dedicated serial touch controller (`/dev/ttyS2`)** — `/etc/profile`
  in the stock rootfs sets `TOUCHSERIAL=/dev/ttyS2` /
  `COMMANDSERIAL=/dev/ttyS2`, which looked promising (a classic
  "piggyback the OEM panel's own serial touch output" pattern). Checked
  every binary in the rootfs (`.so` and executables) for these exact
  strings — **referenced by none of them.** Dead/vestigial config, most
  likely inherited from a shared `ark1668` SDK profile template used by
  other product variants that do have a serial touch controller.

**Authoritative confirmation of the real path:** the stock rootfs's own
`/etc/ts.conf` (`module_raw input`) and `/etc/profile`
(`TSLIB_TSDEVICE=/dev/input/event0`) prove tslib reads touch from a
**kernel evdev device** — i.e. exactly `gt9xx.ko`/`ark1680_ts.ko`, the
two drivers this whole investigation has been testing. There is no third
subsystem left to consider; the full kernel module inventory (22 `.ko`
files total, checked exhaustively) and the stock `vmlinux`'s own strings
confirm no other touch-capable driver exists in this firmware at all.
The remaining open question is purely why the real hardware behind
*one of these two, correctly-identified* paths never responds — which
keeps "panel not physically connected/populated on this bench unit" as
the leading hypothesis.

## Next steps for a 4.19 port

1. Add a DTS node:
   ```
   tsc@e4500000 {
       compatible = "arkmicro,ark1680-ts";  /* new driver, no upstream match */
       reg = <0xe4500000 0x40>;
       interrupt-parent = <&vicl>;
       interrupts = <4>;
   };
   ```
2. Write a new platform driver replicating the register sequence above
   (`ark1680_setup_tsc` init sequence, IRQ handler reading `+0x0c`/`+0x14`,
   debounce/detect constants `40000`/`130`) — either raw-`ioremap` the
   syscon block like the stock driver, or resolve it via the existing
   `pinctrl0`/`sregs@e4900000` node.
3. Decode `TSP_GetXY` before trusting reported coordinates — the above is
   enough to get raw ADC IRQs firing and confirm the hardware responds,
   but not enough for calibrated touch output yet.

---

## The userspace consumer: `tslib`'s `input.so` (2026-07-16)

**Why:** `/etc/ts.conf` (`module_raw input`) means every touch event, from
either kernel driver above, is read by this plugin before reaching
`tslib`'s filter chain (`pthres`→`variance`→`dejitter`→`linear`) and
then `MsnCoreApp`. Decompiled to rule it out as a source of the touch
problem and to document the exact stderr diagnostic it prints if a
kernel driver's evdev capability bits are wrong.

**File:** `mtd6_rootfs/usr/lib/ts/input.so` (8 KB ARM32 shared object,
**not stripped** — full symbol table). `arm-linux-gnueabihf-objdump -d`
+ `readelf -sW` used for recovery.

**Finding: this is stock, unpatched tslib source, not vendor code.**
The symbol table names the original source file `input-raw.c`, exposes
`ts_input_read`/`ts_input_fini`/`mod_init`/`__ts_input_ops` (tslib's
raw-`evdev` plugin), and its `.rodata` contains tslib's stock diagnostic
strings verbatim (`"selected device is not a touchscreen I understand"`,
`"tslib: dropped x = 0"`, `"tslib: dropped y = 0"`,
`"tslib: Unknown event type %d"`). `usr/lib/pkgconfig/tslib-0.0.pc`
confirms the build (`Version: 0.0.2`). No custom logic, no vendor
patches — this binary can be treated as a known quantity; any touch
misbehavior originates upstream of it (kernel driver / evdev bitmap) or
downstream of it (the filter chain / `MsnCoreApp`), not in this plugin.

### `mod_init` (`0xc8c`)

`malloc(36)`, zeroes the module's cached-state fields (offsets
`+16/+20/+24/+28/+32` — last-known x/y/pressure, "checked" flag, "grab"
flag), and wires field `+12` to the plugin's ops vtable (`__ts_input_ops`
— `{ read = ts_input_read, fini = ts_input_fini }`, matching tslib's
generic `struct tslib_module_info { next, ops }` layout).

### `ts_input_read` (`0x790`) — capability probe (first call only)

State field `+28` is a tristate cache (`0` = unchecked, `-1` = "already
rejected, don't retry", else = normal). On the very first call it probes
the fd with three `ioctl()`s against `/dev/input/eventN`:

| Call | Request code | Meaning |
|---|---|---|
| `ioctl(fd, EVIOCGVERSION, &ver)` | `0x80044501` | evdev driver version sanity check |
| `ioctl(fd, EVIOCGBIT(0, 32), bits)` | `0x80204520` | bitmap of supported **event types** (`EV_SYN`/`EV_KEY`/`EV_ABS`/…) |
| `ioctl(fd, EVIOCGBIT(EV_ABS, 64), bits)` | `0x80404523` | bitmap of supported **absolute axes** |

It then requires, from the `EV_ABS` bitmap: bit 0 (`ABS_X`) **and** bit 1
(`ABS_Y`) **and** bit 24 (`ABS_PRESSURE`) all set. If any `ioctl` fails
or any bit is missing, it prints `"selected device is not a touchscreen
I understand"` to stderr, permanently caches `-1` in `+28` (so every
later call is a silent instant no-op returning 0 samples), and the
plugin is effectively dead for that fd for the rest of the process.

**Practical upshot for this investigation:** if touch is ever tested live
again, grepping stderr/dmesg-adjacent app logs for that exact string is
a fast, unambiguous way to tell "kernel driver never advertises
`ABS_X`/`ABS_Y`/`ABS_PRESSURE` capability bits" apart from "kernel driver
advertises them fine but never posts events" — the former fails here in
userspace on the very first read, the latter passes this check and just
blocks in `read()` forever. Neither `ark1680_ts.ko` nor `gt9xx.ko`'s
capability-bit setup has been checked against this specific requirement
yet.

### `ts_input_read` — steady state

Once the probe passes, each call loops `read(fd, buf, 16)` (one
`struct input_event`: `sec, usec, type, code, value`) up to the
caller-requested sample count, updating cached x/y/pressure on
`EV_ABS` events (`ABS_X`, `ABS_Y`, `ABS_PRESSURE`/code `0x18`) and
emitting a `struct ts_sample` on `EV_KEY`/`SYN`-driven boundaries. Unknown
event types produce the `"tslib: Unknown event type %d"` stderr message
(the only reachable `fprintf` in the function) but do not abort the
read loop.

### `ts_input_fini` (`0x780`)

`free()`s the module struct, returns 0. Trivial, no surprises.

---

## `MsnCoreApp` segfault — likely root cause found and fixed (2026-07-11)

Since neither touch driver ever sees hardware activity, and `/dev/ttyHS0`
(MCU) is completely silent, the `MsnCoreApp` segfault on `start_msn`
(originally Blocker 2 in `HANDOFF_bootlog_v6_review.md`) became worth
investigating directly — it may be load-bearing for touch too (see
"MCU role" retraction in `MCU_ADAPTERS.md`: the MCU may only enable the
CBT16211A touch-bus switch after a successful handshake with
`MsnCoreApp`, which never happens if it crashes immediately).

**Traced the crash's likely cause via static disassembly** (unstripped
`Prado firmware dump/mtd6_rootfs/usr/bin/MsnCoreApp`, `libarkcmn.so`,
and the stock `vmlinux.elf`, using `arm-linux-gnueabihf-objdump -C` for
demangled disassembly — no live debugger available):

1. `main()` → `MsnCoreApp::MsnCoreApp()` → `onFirstInit()`. The very
   first substantive call in `onFirstInit()` (9224 bytes, huge function)
   is `arkapi_get_screen_info()` (`libarkcmn.so`).
2. `arkapi_get_screen_info()` does `open("/dev/ark_display", O_RDWR)`
   then `ioctl(fd, 0xc004a01d, &info)`, validating only `info[0]`
   (must be 0-7). Returns -1 on any failure (missing device, ioctl
   error, or out-of-range id) — safely checked by its own logic.
3. **`/dev/ark_display` is created by the stock kernel's
   `ark_display_drv.c`** (`ArkPro Reference/kernel/drivers/ark/display/`),
   registered via `misc_register()` — a vendor-specific misc device,
   separate from the standard `ark1668_lcdfb` framebuffer driver our
   4.19 port actually uses. **Confirmed absent from `linux-arkmicro`'s
   source tree, `kernel_dot_config`, and every boot log captured so
   far.** It was simply never ported.
4. Back in `onFirstInit()`, the branch on `arkapi_get_screen_info()`'s
   result is inverted from the naive expectation: **success jumps past**
   a huge block; **failure falls straight through** into it — a ~9KB
   fallback/default-initialization path that stock firmware (which has
   the real device) never normally exercises. That's a strong candidate
   for an effectively untested code path now running unconditionally on
   every boot of this rebuilt firmware.
5. Independent corroboration: `MsnFirstInit` (a *different* binary, run
   earlier from `/etc/profile`) hits the same missing-device condition
   via what's presumably the same or a related call, and falls back to
   a wrong panel-size default — the bogus `"set display inch: QSize(154, 86)
   6.94433"` seen in every boot log, vs. the real ~5.5"/120×72mm panel.

**Recovered the exact ioctl encoding and reply format** by disassembling
the *real* handler in the stock kernel binary (not the generic vendor
reference header, which turned out to be from an incompatible/older SDK
revision — its documented command range topped out one below the command
we needed): `mtd5_kernel/extracted/vmlinux.elf`, `ark_disp_ioctl` @
`0x802d9fd8`, the case for cmd `0xc004a01d` @ `0x802da7d4`. Command
decodes to type `0xa0` (matches `ARKDISP_IOCTL_BASE` from
`ArkPro Reference/userspace/display.h`) + nr 29, read|write, 20-byte
payload. The handler copies 5×`u32` sourced from the kernel's
`screeninfo_param` global (the same 120-byte runtime struct already
identified during the GT911/LCD-timing work in this doc) — word 0 is the
screen id (the only field the caller validates), the rest are
best-effort panel geometry.

**Fix implemented:** `Limcet Hardware/ark_display.c` — a minimal
`/dev/ark_display` misc device (no DTS node needed; pure software
`misc_register()`, same pattern as the stock driver) implementing just
`ARKDISP_GET_SCREEN_INFO`, returning `screen_id=0` (matches this unit's
`arkdata.ini` `ScreenId=0`) and `800×480`/`120×72mm` geometry (matches
`docs/DISPLAY_SUBSYSTEM.md`'s `ScreenId=0` entry and the ~5.5" real panel size).
Wired into the build tree (`drivers/misc/`, new `CONFIG_ARK_DISPLAY=y`
Kconfig/Makefile entries) — compiles clean (`W=1`, zero warnings), full
`zImage` rebuild succeeded.

**v9 hardware test — driver registered, but bug in the fix itself
(2026-07-11):** `docs/new kernel bootlog new uboot v9.txt` confirmed
`ark_display: registered /dev/ark_display` at boot, but `MsnFirstInit`
and `MsnCoreApp` were **completely unchanged** — identical
`QSize(154, 86) 6.94433`, identical immediate segfault. Root cause:
**my own ioctl macro was wrong.** The vendor's userspace convention
(confirmed across every command in `ArkPro Reference/userspace/display.h`)
uses `unsigned long` (4 bytes) as the `_IOWR()` type argument for
*every* `ARKDISP_*` command, regardless of the command's real payload
size — the encoded "size" field in the ioctl number is decoupled from
what the driver actually `copy_to_user()`s at runtime. My driver used
`struct ark_screen_info` (20 bytes) as the macro argument instead,
which silently produced a *different* command number
(`0xc014a01d` vs. the real `0xc004a01d`) — so the app's `ioctl()` call
never matched my `switch(cmd)`, fell to `default`, returned `-ENOTTY`,
and `arkapi_get_screen_info()` failed exactly as if the device didn't
exist at all. Fixed: macro now uses `unsigned long` (matching the
vendor convention) while the handler still `copy_to_user()`s the full
20-byte struct, matching the real kernel's actual runtime behavior.
Verified via disassembly of the rebuilt `.o` that the compiled
comparison now uses the correct `0xc004a01d`. Kernel rebuilt clean,
**re-flash and re-test needed** — this exact failure mode (device
registers, ioctl silently mismatches) is easy to miss without checking
the compiled constant directly, which is now part of the verification
step for any future driver work here.

**Cross-checked against `docs/boot log.txt` (full working stock boot,
never analyzed line-by-line before) — confirms the fix's values and
raises a new question about the crash location:**

- `[7.791] MSNCoreApp onFirstInit` → `[7.795] ark screen type: 0` →
  `[7.796] display size: QSize(800, 480) ...` — `ark screen type: 0` is
  literally the `screen_id` `arkapi_get_screen_info()` returns, and
  `QSize(800, 480)` is `width_px`/`height_px`. Both match
  `ark_display.c` exactly. A third independent confirmation of `120×72`
  mm also appears later (`screen_width_phy=120 screen_height_phy=72`).
  High confidence the chosen constants are correct.
- **But**: stock has three prints *before* `onFirstInit` even runs —
  `MSNCoreApp start` / `MSNCoreApp exception init` / `MSNCoreApp
  running` (5.35-5.38s) — that have **never appeared in any of our
  boot logs**. Our crash produces zero app-level output at all. If the
  crash were specifically inside `onFirstInit`'s fallback path, these
  three earlier prints should still succeed first. Their total absence
  means either the real crash point is earlier than previously assumed,
  or (more likely) stdout is buffered and never flushed before the
  segfault kills the process, hiding output that did happen.
- `QWS_ARK_TOUCH_DEVICE undefined, touch input disabled` — appears in
  stock too, expected/harmless, not a regression. Real touch
  consumption in stock is a separate, later line:
  `[10.748] open touch monitor dev: /dev/input/event0 41` (during
  plugin loading, well after `onFirstInit`) — the line to watch for
  once `MsnCoreApp` gets further.

**`strace` now available.** Cross-compiled upstream strace v7.1
statically for the target (`tools/strace/`, wired into
`build_bootable_sdcard.sh`'s diagnostic-tools install alongside
`i2c-scan`/`ark-ts-test`/`lcd-test`) — no host-gdb needed to trace the
segfault; `strace -f start_msn` (or `strace -f MsnCoreApp -qws`) on
the next boot should show the exact syscall sequence up to the
`SIGSEGV`, which is normally enough to identify the crashing function
without any disassembly. This supersedes the `user_debug=8` approach
below as the primary tool for the next test — that one only gives a
faulting address, `strace` gives the call sequence leading up to it.

**Enabled exact crash-address reporting for the next test (secondary,
kept as a fallback).** The kernel has `CONFIG_DEBUG_USER=y`;
ARM's `__do_user_fault()` prints the exact faulting PC/address to
`dmesg` on a userspace SIGSEGV, gated by the `user_debug` boot param
(`UDBG_SEGV = 1<<3 = 8`, `arch/arm/include/asm/system_misc.h`) — not a
runtime-toggleable sysfs value, has to be on the kernel command line.
Added `user_debug=8` to both `bootargs=` lines in
`generate_uenv_txt()` in `build_bootable_sdcard.sh` — next SD build
will have it. Next test: reflash, run `start_msn`, then `dmesg | tail`
— should show the exact crashing address directly, resolving the
"where exactly" question without needing gdb/core dumps at all.

**Still open regardless of the ioctl fix:** field order for the 4
geometry words beyond `screen_id` isn't confirmed byte-exact against
stock (only `screen_id` was traced with certainty as the field
`arkapi_get_screen_info()` validates) — if `MsnFirstInit`'s computed
`QSize` still looks wrong after a correct ioctl match, that's the next
place to check. If `MsnCoreApp` still segfaults even with the ioctl
matching, the crash is elsewhere in that ~9KB fallback path or beyond
it.

**Cross-check against `docs/boot log.txt` (the actual working stock
boot log) confirms the fix's target values are right:** stock shows
`set display inch: QSize(120, 72) 5.50956` — 120×72mm matches
`ark_display.c`'s `mm_width`/`mm_height` exactly. Good independent
confirmation the geometry constants chosen are correct, even before the
ioctl-match bug was found.

## A second, separate subsystem: `arktool_reg_init` — PORTED 2026-07-18

`docs/boot log.txt` also has a line never seen in any of our boot logs:
`[    1.340000] arktool display reg init`, printed **before rootfs
mount** — genuinely early kernel boot. Traced it in `vmlinux.elf`:

- `arktool_reg_init()` (`0x802ff390`) `ioremap()`s **both**
  `0xe0500000` (LCD controller — same block `ark1668_lcdfb` also maps)
  and `0xe4900000` (syscon/pinmux — same block from the touch driver
  work), then builds a table of ~60 raw register pointers into a global
  struct at `0x805fd29c`.
- Called from `ark1680_uart_startup()` — a shared UART driver's
  `.startup()` callback. **Trigger port pinned down via Ghidra
  decompilation of `ark1680_uart_probe()` (2026-07-18): sub-port index
  0 of the 4-port `ark1680-uart` platform device, which is `uart0` at
  `0xe4200000` — the debug console, `ttyS0` on this tree.** Not
  `ttyS2`/`ttyS3`/`ttyHS0`/`ttyHS1` as originally guessed. This also
  explains the "before rootfs mount" timing: the console UART gets its
  first open essentially immediately at boot. The 4 `ark1680-uart`
  ports (`0xe4200000`/irq20, `0xe4e00000`/irq21, `0xe8000000`/irq44,
  `0xe8100000`/irq45) turn out to already match this tree's own
  `ark1668.dtsi` `uart0-3` nodes byte-for-byte — no DTS changes were
  needed, only the missing driver-side hook.
- Paired with `ark_tool_handle()` right next to it — checksummed
  framing (signature byte `0xAC`, running XOR checksum, command IDs
  `0xB1`-`0xD8` via a 40-entry jump table), matching
  `docs/KERNEL_REFERENCE.md`'s already-documented **"arktool" binary
  protocol**: mostly Ypbpr/DDS/PLL analog-video-output configuration
  and register telemetry readback, plus boot-animation sync and MCU
  health monitoring per that doc. **Independently confirmed unrelated
  to touch**: `ark1680_ts.ko`'s own object file has zero references to
  this struct or any arktool symbol — it does its own independent
  pinmux pokes. Not the CBT16211A touch-switch trigger; see
  `tools/mcu-handshake/README.md` for that separate investigation.

**Ported** in `linux-arkmicro` (`drivers/misc/ark_tool.c`, new; one-line
hook in `drivers/tty/serial/ark_uart.c`'s `pl011_startup()`;
`CONFIG_ARK_TOOL=y` in `ark1668_defconfig`): the register table (all
~60 pointers, offsets fully confirmed) and a `/proc/arktool` interface
that validates incoming frames (signature + checksum + command-ID
range) and logs them. **Deliberately does not yet act on any received
command**, including `0xB7` (the one command confirmed to perform real
pinmux writes to `0x54`/`0x68`/`0x70`) — disassembly confirmed *which*
registers it touches but not the exact payload-byte-to-value mapping,
and guessing at that risked writing wrong data into a live pinmux
register. Extending this needs either a live capture of a real `0xB7`
frame from the MCU, or further disassembly of that command's exact
payload unpacking. Built clean against the 4.19 tree, not yet boot-
tested on real hardware — `cat /proc/arktool` after boot is the
quickest way to confirm it initialized (should show `ready`, both
`ioremap`'d base addresses, and frame/checksum counters).