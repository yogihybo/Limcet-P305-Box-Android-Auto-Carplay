# Master pin list — everything known, 2026-07-13

Consolidates every pin assignment confirmed this session (live debugfs +
DTS cross-check + stock-kernel disassembly), plus the hard lesson from
the GPIO brute-force incident: **the pinctrl DTS and live debugfs
pinmux-pins view are not a complete picture.** Several real, actively-used
peripherals never appear there at all because they're configured via
fixed/early pad routing outside the standard Linux pinctrl framework.
Treat any pin not explicitly listed here as **unverified, not confirmed
free** — "no claim found" is not the same as "safe."

Global pin numbering = `bank * 32 + offset` (confirmed via
`pinctrl-ark.c:25,773`, `MAX_PIN_PER_BANK=32`, `ARK_PBANK_0=0` etc.).
This matches `/sys/class/gpio/gpioN` 1:1 on this board (confirmed via
live `gpio-ranges` debugfs — direct mapping, no offset).

## Confirmed claimed — visible in pinctrl DTS + live debugfs

| Pins (global) | Bank/offset | Function | Source |
|---|---|---|---|
| 2-25 | PBANK_0 2-25 | LCD RGB888 r0-r7/g0-g7/b0-b7 (**active**) | `ark1668-pinctrl.dtsi:45-70`, live-confirmed muxed to `e0500000.lcd` |
| 26-29 | PBANK_0 26-29 | LCD base: de/clk/vsync/hsync (**active**) | `ark1668-pinctrl.dtsi:37-42` |
| 39-52 | PBANK_1 7-20 | NAND data/cle/ale/ren/wen/rb0/cen0 | `ark1668-pinctrl.dtsi:89-106` — **not shown as claimed in live debugfs** (see caveat below) |
| 62-63 | PBANK_1 30-31 | **uart0 — the console UART this entire investigation used** | `ark1668-pinctrl.dtsi:109-114` — **also invisible in live debugfs** |
| 64-65 | PBANK_2 0-1 | uart1 rx/tx | `ark1668-pinctrl.dtsi:117-123` |
| 66-67 | PBANK_2 2-3 | uart2 rx/tx | `ark1668-pinctrl.dtsi:125-131` |
| 68-69 | PBANK_2 4-5 | uart3 rx/tx | `ark1668-pinctrl.dtsi:133-139` |
| 70-71 | PBANK_2 6-7 | **hw `&i2c0`** (DesignWare, clean/unconflicted; ACKs seen at 0x10/0x11) | live-confirmed, `boot_experiment_log.md` |
| 72-75, 77-78 | PBANK_2 8-11,13-14 | LCD LVDS group (defined but **inactive** — RGB888 selected instead; electrically free right now but reserved if LVDS mode is ever selected) | `ark1668-pinctrl.dtsi:73-86` |
| 79 | PBANK_2 15 | pwm1 (backlight-adjacent; treat as live given "Open backlight pwm" at every `start_msn` run) | `ark1668-pinctrl.dtsi:158-161,213-216` |
| 80 | PBANK_2 16 | pwm2 | `ark1668-pinctrl.dtsi:218-221` |
| 81 | PBANK_2 17 | pwm3 — **also actively GPIO-claimed as `lcd-power-control-gp`** | `ark1668-pinctrl.dtsi:223-226`, live `/sys/kernel/debug/gpio` |
| 82-84 | PBANK_2 18-20 | i2s1 sync/sadata/bclk (**active** — I2S/audio confirmed working) | `ark1668-pinctrl.dtsi:174-191`, `AUDIO_SUBSYSTEM_INVESTIGATION.md` |
| 97-99 | PBANK_3 1-3 | SPI clk/rxd/txd | `ark1668-pinctrl.dtsi:164-171` |
| 119-120 | PBANK_3 24,23 | uart4 rx/tx | `ark1668-pinctrl.dtsi:141-146` |
| 123-124 | PBANK_3 28,27 | uart5 rx/tx | `ark1668-pinctrl.dtsi:149-154` |
| 127 | PBANK_3 31 | i2s1 mclk | `ark1668-pinctrl.dtsi:193-196` |
| 128-129 | PBANK_4 0-1 | LCD LVDS cn/cp (inactive, same caveat as 72-78) | `ark1668-pinctrl.dtsi:81-82` |
| 130 | PBANK_4 2 | i2s1 sadata-in (**active**) | `ark1668-pinctrl.dtsi:198-201` |

## Confirmed claimed — GPIO-level only (not a pinctrl "function", plain `gpio_request`)

From live `/sys/kernel/debug/gpio` (2026-07-13):

| GPIO | Consumer | Notes |
|---|---|---|
| 0 | `rn6752_reset` | camera decoder reset |
| 1, 76 | `usb_id` | USB OTG ID pin, two GPIOs |
| 2 | `i2c-gpio-0` SCL | **= LCD r0, conflicted, see `I2C_GPIO0_LCD_PIN_CONFLICT.md`** |
| 3 | `i2c-gpio-0` SDA | **= LCD r1, conflicted** |
| 5 | `detect` | **reverse-gear/carback trigger input** — confirmed via Ghidra decompile of `ark_carback_probe`, 2026-07-13 session 3 (see below) |
| 81 | `lcd-power-control-gp` | also pwm3's pinctrl pin, dual use |
| 117, 126 | `usb_pwr` | USB power switch, two GPIOs |

## MMC/SD and USB — very likely dedicated, non-multiplexed pads, not in the PBANK_0-4 space at all

**Updated 2026-07-13, cross-checked against U-Boot source** (which also
has working LCD/USB/MMC, giving an independent second implementation to
compare against). `board_mmc_init()`
(`u-boot/board/arkmicro/ark1668_limcet_p305/ark1668.c:85-89`) calls only
`ark_dwmci_init("ARK_MMC0", 0xec400000, 4, 0)` — base address and bus
width, **zero pin/pad configuration code**. `drivers/usb/musb/ark_musb.c`
shows the identical pattern — no pad/pinmux config at all. Neither
codebase ever touches a pinmux for MMC or USB, in either U-Boot or
Linux, on top of Linux's `mmc0`/`mmc1` (`ark1668.dtsi:532-558`) having no
`pinctrl-0` property and no `cd-gpios` anywhere in the DTS.

**Most likely explanation: MMC and USB are on dedicated, non-multiplexed
silicon pads, physically outside the shared `PBANK_0-4` GPIO/pinmux
matrix entirely** — a common SoC pattern for high-speed/critical
interfaces. If true, this means the earlier theory in this doc (that the
incident hit a `PBANK_2`/`PBANK_3` pin adjacent to MMC's padcfg register
offset) was **likely wrong** — MMC's real pins probably aren't
reachable via `/sys/class/gpio` at all, so the brute-force scanner
couldn't have driven them directly.

**This means the actual cause of the 2026-07-13 incident
(`mmc0: card e126 removed` → EXT4 journal abort → full filesystem
corruption, recovered by power-cycle) is now genuinely uncertain, not
narrowed down.** Candidates: a transient electrical disturbance from
rapid GPIO switching activity (current draw / ground bounce affecting a
marginal SD card connection on the dev setup), or a coincidental
mechanical/socket contact issue unrelated to which pins were driven.
**Do not treat this as resolved or as evidence the tested range is
safe** — the honest state is "cause unknown," which is worse for
planning purposes than either a confirmed pin conflict or a confirmed
false alarm would be.

**Practical consequence unchanged: treat the entire
85-96/100-116/118/121-122/125 range as unverified-unsafe**, and don't
attempt further live GPIO brute-forcing there without a real
schematic/datasheet or a backed-up SD card to make failures cheap to
recover from.

## The core lesson (why this doc exists)

Three different peripherals — **uart0 (console!), NAND, and MMC** — are
all real, always-critical, and all **invisible** to both:
1. The static `ark1668-pinctrl.dtsi` group list (uart0/NAND are in
   there but never *selected*/applied via `pinctrl-0` on any node we
   found, or use a different application path — TODO: confirm how
   they're actually applied if not via the standard mechanism); MMC
   isn't in the pinctrl DTS at all.
2. Live `/sys/kernel/debug/pinctrl/*/pinmux-pins` debugfs, which only
   tracks pins claimed via the standard late `pinctrl_select_state()`
   API.

**"MUX UNCLAIMED" in debugfs means "not claimed via the standard
pinctrl API" — it does not mean "electrically free."** Any future live
GPIO probing must not rely on that signal alone. The only pins in this
document with genuine confidence behind "actively used, don't touch"
are the ones with a second, independent confirmation (a live
`/sys/kernel/debug/gpio` GPIO-level claim, a working feature we can
observe — audio, video, this exact serial link — or stock-kernel
disassembly).

## Still fully unverified (no claim found anywhere, but not incident-tested)

Global pins not listed above at all: 30-38, 53-61, 85-96, 100-116, 118,
121-122, 125 minus whatever MMC actually uses within 85-125ish (see
above — do not treat this whole remaining set as safe either, given the
demonstrated blind spot).

## Stock 3.4 kernel board-file static disassembly (2026-07-13, new method)

**New, safer methodology validated this session**: disassembling the stock 3.4
kernel's `vmlinux.elf` (`Prado firmware dump/mtd5_kernel/extracted/vmlinux.elf`
— unstripped, has `System.map`) directly gives the board file's hardcoded
GPIO/bus/platform-data assignments, with **zero hardware risk** — no live
probing, no telnet session, no risk of repeating the mmc-corruption incident.
It fully reproduced every fact the live `i2c-scan` investigation found, from
the binary alone:

- `ark1680_machine_init` (`0x8059f188`) calls `ark1680_add_device_i2c` (hw
  `i2c0`, 1 device) and `analog_i2c_add_device_i2c`/`_i2c3` (bus "1"/"3") with
  pointers into one static `struct i2c_board_info[]` table at `0x805b389c`
  (stride `0x28`/40 bytes — confirms the struct layout for this kernel build).
  `ark1680_add_device_audio` (`0x8059efb4`) separately calls
  `analog_i2c_add_device_i2c_2` (bus "2") pointing at the **same table's index
  0**.
- Reading the raw bytes of that table (`objdump -s -j .text
  --start-address=0x805b3894 --stop-address=0x805b3a60 vmlinux.elf`):
  - index 0 (`0x805b389c`): `"drv_bd37033"`, addr `0x41` → registered on
    **bus "2" (i2c-gpio2)**, count 3 — matches live `i2c-2: 0x41 XX` exactly.
  - index 3 (`0x805b3914`, table+0x78): `"Goodix-TS"`, addr `0x5d` →
    registered on **hw i2c0**, count 1 — matches the stock board file
    unconditionally declaring GT911 there (already known), now confirmed via
    raw struct bytes not just symbol names.
  - index 8 (`0x805b39dc`, within table+0xa0, count 6): `"rn6752"`, addr
    `0x2c` → registered on **bus "1" (i2c-gpio1)** — matches live `i2c-1:
    0x2c XX` exactly. Indices 4-7,9 in this 6-entry range are zero/unused.
  - index 10 (table+0x190, `analog_i2c_add_device_i2c3`): count 0 — bus "3"
    is a no-op, nothing registered.
  
  **Net effect: this single static disassembly, done cold with no live
  device access, reproduces 100% of the bus-placement ground truth the live
  `i2c-scan` session spent a whole investigation establishing.** This should
  now be the *first* step for any remaining unresolved peripheral
  (touch/MCU-UART/SWC/backlight/etc.) before any live probing is considered.

- **New finding, previously undocumented**: `customer_gpio_init`
  (`0x80018834`, called at the end of `ark1680_machine_init`) does
  `gpio_request(95, "apple_encpy_ic_rst")` then
  `gpio_direction_output(95, 1)` — **GPIO 95 is a reset line for an "apple
  encryption IC"** (likely a CarPlay/MFi auth chip), driven high
  unconditionally at boot. Not in any DTS or prior doc. Global pin 95 = bank 2
  offset 31 (95 = 2*32+31), i.e. `PBANK_2` offset 31 — **not yet
  cross-checked against the LCD/pinctrl pin-group tables**, do that before
  assuming it's free in the 4.9 DTS.

- **Structural finding — touch (`ark1680_ts`, the resistive-ADC driver)
  bypasses GPIO/pinctrl entirely**, per the reconstructed source
  (`Limcet Hardware/ark1680_ts.c:56-67,246,263`): it maps the shared
  syscon/pinmux block directly at fixed physical address `0xe4900000` and
  clears specific bits — `ARK_SYS_PINMUX0` (offset `0x140`) bit 22, and
  `ARK_SYS_PINMUX1` (offset `0x144`) bit 14 — to route its ADC pads, with no
  `gpio_request`/`pinctrl_select_state` call anywhere. Its IRQ comes from a
  platform resource (`platform_get_irq`), not a GPIO. **This is the same
  "invisible to both static pinctrl DTS and live debugfs" pattern already
  flagged above for NAND/uart0/MMC** — confirms that pattern is common on
  this SoC for early/critical blocks, not a one-off. Any 4.9 port of touch
  needs to replicate this exact register poke (not a pinctrl group), or
  touch will silently fail to route to its pads even if the driver otherwise
  loads and probes cleanly.

### Continued (2026-07-13, session 2): hsuart/carback/IR device identities + a fourth mux layer

Disassembled `ark1680_add_device_hsuart` (`0x8059ee30`), `_carback`
(`0x8059f160`), `_ir` (`0x8059f174`) and their `platform_device` structs.

**Confirmed device names (high confidence — read directly from the string
table, same technique as the i2c names above):**
- `ark1680_add_device_hsuart`'s device is named **`"ark1680-hsuart"`** — this
  is the actual UART hardware controller behind `/dev/ttyHS0` (the MCU link
  used by `libMcuCenter.so`/`McuType=6`, see `MCU_ADAPTERS.md`). Its pad
  setup calls `ark_sys_pad_config(reg=0x1f0, mask=0xf, shift=2, val=0xf)`
  and `(reg=0x1f0, mask=0xf, shift=6, val=0xf)`.
- `ark1680_add_device_carback`'s device is named **`"ark_carback"`** —
  reverse-camera trigger.
- `ark1680_add_device_ir`'s device is named **`"ark_nec_sw_remote"`** — a
  software (bit-banged/edge-IRQ) NEC-protocol IR remote decoder, not a
  hardware IR block.

**New structural finding — a fourth, distinct pin-mux mechanism.**
`ark_sys_pad_config(reg, mask, shift, val)` (called throughout the board
file for i2c/uart/hsuart/audio/pwm/mci) does a read-modify-write of a
multi-bit field at a fixed `reg` offset (`0x1e0`, `0x1e4`, `0x1f0` seen so
far) within the same fixed physical `sys`/pinctrl0 MMIO block `ark1680_ts.c`
already showed being poked directly for touch (`ARK_SYS_PINMUX0`/`_1` at
`0x140`/`0x144`). So there are now **four** separate places pin/pad routing
lives on this SoC, and stock 3.4 only ever uses the first three — the 4.9
port's generic Linux pinctrl framework (`pinctrl-ark.c`, PBANK/DTS groups) is
a *different, newer* software abstraction over what is presumably the same
underlying hardware registers:
1. `ark_sys_pad_config()` raw field writes (i2c/uart/hsuart/audio/pwm/mci) —
   stock 3.4's primary mechanism, invisible to both the 4.9 DTS and live
   pinctrl debugfs.
2. Direct `ARK_SYS_PINMUX0/1` bit-clears (touch ADC pads) — same block,
   different offsets, also invisible to both.
3. Plain `gpio_request()`/`gpio_direction_*()` (e.g. GPIO 95
   `apple_encpy_ic_rst`) — visible in live `/sys/kernel/debug/gpio`, but not
   in pinctrl debugfs.
4. The 4.9 kernel's `pinctrl-ark.c` + DTS `ark,pins` groups — visible in
   both the DTS and live pinmux-pins debugfs, but **only for pins some DTS
   node actually selects via `pinctrl-0`** (this is the mechanism already
   covered everywhere else in this doc).

**Practical implication: "not found in the DTS or live pinctrl debugfs" is
not evidence a peripheral's pins are unconfigured or free — it may just mean
stock configures them through mechanism 1 or 2 instead**, which the 4.9 port
needs to replicate as raw register pokes (in the driver, or in an early
board-init hook) rather than expecting a pinctrl DT group to cover it.

**Unconfirmed — flagged rather than guessed, following this project's own
evidence-level convention (see `MCU_ADAPTERS.md`'s header):** attempts to
read GPIO/IRQ numbers out of the raw bytes of the `carback`/`ir`/`hsuart`
`platform_device` structs themselves (beyond `name`/`id`) are **not
reliable by hand** — `struct device`'s exact size on this kernel build is
config-dependent (`CONFIG_NUMA`/`CONFIG_CMA`/etc. all change field offsets),
so a byte offset that looks like a plausible small int (e.g. a `98` or `5`
found near these structs) could be genuine platform data or could be an
adjacent, unrelated static array — indistinguishable without either the
actual board-file source or a type-aware disassembler (Ghidra with the
kernel's struct definitions imported) rather than raw `objdump -s`. **Do not
treat any specific GPIO/IRQ number for carback/ir/hsuart's platform_data as
confirmed until re-derived that way.** No stock 3.4 board-file *source* was
found anywhere in this repo or the `linux-arkmicro` reference/build trees to
shortcut this (both only contain 4.x-era DT-based sources) — the compiled
`vmlinux.elf` is the only ground truth available for stock 3.4.

### Continued (2026-07-13, session 3): Ghidra-verified facts — GPIO 5 resolved, hsuart resources confirmed against the 4.9 DTS

Set up Ghidra 12.1.2 headless (`/home/osboxes/tools/ghidra`, `JAVA_HOME=/home/osboxes/tools/jdk/jdk-21.0.11+10`) against `vmlinux.elf`, ran full auto-analysis, then decompiled the actual driver **probe functions** (not just the board-file registration calls) for `ark_carback` and `ark1680-hsuart`. This is a materially stronger technique than reading raw struct hex by hand — the decompiler shows *how the driver code itself uses each field*, which resolves ambiguity no amount of hex-reading can: a byte offset is proven meaningful the moment you see the driver read it and hand it to `gpio_request`/`platform_get_irq`/etc., instead of being merely "a plausible small integer nearby."

**GPIO 5 resolved — reverse-camera trigger input.** `ark_carback_probe` (`0x80416880`) contains
`piVar1[0x19] = *(int *)(param_1 + 0x50);` followed by `__gpio_get_value()`,
`__gpio_to_irq()`, and `request_threaded_irq(irq, ark_carback_intr_handler, 0, 3, "carback", ...)`
on that value. Combined with the raw hex read that already found the literal value `5` at
`platform_device + 0x50` for this device, this **confirms GPIO 5 is the reverse-gear/carback
trigger input** — resolving the open item this doc has carried since the live-GPIO investigation
(`/sys/kernel/debug/gpio` labeled it only `"detect"`, unidentified). It's edge/level-triggered via
`gpio_to_irq`, not polled.

**hsuart resources decoded and independently confirmed by the 4.9 DTS.** `ark1680_hsuart_probe`
(`0x802fcf08`) loops `platform_get_resource(pdev, IORESOURCE_MEM, i)` /
`platform_get_irq(pdev, i)` for `i = 0, 1` — i.e. **one `platform_device` registers two
independent UART instances** (matches the `num_resources=4` found earlier by hex-reading the
struct). Reading the actual 4-entry `struct resource[]` array (`0x805c86c8`, `objdump -s`):

| Index | Type | Value |
|---|---|---|
| 0 | MEM | `0xe4f00000`-`0xe4f000ff` |
| 1 | IRQ | 22 |
| 2 | MEM | `0xe4800000`-`0xe48000ff` |
| 3 | IRQ | 28 |

**Cross-checked against the 4.9 DTS (`linux-arkmicro Reference/linux/arch/arm/boot/dts/ark1668.dtsi:488-505`) — it matches exactly**: `uart4: serial@e4f00000` and `uart5: serial@e4800000`, both `compatible = "arkmicro,ark-hsuart"` (the same custom driver, not generic 8250 — confirms `ark1680_hsuart_probe` is this driver's ancestor), aliased as `hsserial0`/`hsserial1` in the DTS's `aliases` node. **This means the 4.9 port's hsuart/MCU-UART node addressing is already correct** — `uart4` (`0xe4f00000`) is the MCU link (`/dev/ttyHS0` equivalent). If MCU communication isn't working on the 4.9 kernel, the bug is elsewhere (IRQ routing, clock setup, pinctrl group `pinctrl_uart4`, or the driver itself) — not the base address/IRQ mapping, which is now proven right.

**`ark_nec_sw_remote` (IR) — not pursued further, not applicable to this unit.** Its
platform_data GPIO wasn't found via Ghidra this session (no data cross-reference to its name
string, unlike carback/hsuart, suggesting its driver is a `.ko` not present in this `vmlinux.elf`
or wired up differently) — but this is now moot: **confirmed this hardware revision has no IR
sensor populated**, so `ark_nec_sw_remote` is dead/inapplicable code on this unit and not worth
any further reverse-engineering time.

### Next steps for this method
- [x] Device *names* decoded for `carback`/`ir`/`hsuart` (see above) — `id`
      field and pad-config calls also confirmed.
- [ ] Get real GPIO/IRQ numbers for `carback`/`ir`/`_pwm_backlight`'s
      platform_data — requires loading `vmlinux.elf` into Ghidra/IDA with
      ARM Linux 3.4 `struct device`/`struct platform_device`/`struct
      resource` type definitions imported, so field offsets are computed
      correctly instead of guessed from raw hex. Manual `objdump -s` hex
      reading is not reliable enough for this (see caveat above) — don't
      repeat that approach without type info backing it.
- [ ] Confirm what `Goodix-TS`/index 3's `platform_data` pointer holds —
      resistive touch is what's actually `insmod`ed per `rcS`, but the board
      file *does* register a Goodix client unconditionally; its platform_data
      likely has an IRQ-GPIO/reset-GPIO pair worth extracting even though it's
      dormant on this unit. Same Ghidra-with-types caveat applies.
- [ ] Find the two more `ark_sys_pad_config` reg values already seen
      (`0x1e0` fields at shift 4/8/12/16, `0x1e4` fields at shift 18/20) and
      map every shift/val combination for every peripheral that calls it
      (mci, i2c, uart, audio, pwm) into one consolidated table — would fully
      characterize mechanism 1 (the primary stock pin-mux mechanism) instead
      of the partial picture gathered so far.
- [ ] Cross-check GPIO 95 against the `PBANK_2` pinctrl groups already in
      this doc (pwm1/pwm2/pwm3/i2s1 all live in `PBANK_2` too) before relying
      on it being free.

## Open items

- [ ] Confirm MMC/USB really are dedicated non-multiplexed pads (e.g.
      via a SoC datasheet/pad table) rather than assuming from the
      absence of pinmux code in U-Boot/Linux.
- [ ] The 2026-07-13 incident's actual cause is unresolved — worth
      revisiting if it's safe to retry a *much* more cautious live test
      (one pin pair at a time, SD card backed up first) to determine
      whether it reproduces at all.
- [ ] Confirm how/whether `pinctrl_nand`/`pinctrl_uart0` actually get
      applied if not through the mechanism live debugfs tracks — could
      reveal the same "early/fixed pad, not pinctrl-API-visible" pattern
      applies more broadly than currently documented.
- [x] Trace GPIO `5`'s `detect` consumer — resolved 2026-07-13 session 3: reverse-gear/carback trigger input (Ghidra decompile of `ark_carback_probe`).
- [ ] Revisit the GT911/rn6752/BD37033 real-wiring question with this
      corrected understanding once a safer verification method
      (schematic, or single-pin tests with the SD card backed up) is
      available.

## Cross-references

- `docs/I2C_GPIO0_LCD_PIN_CONFLICT.md` — the LCD/i2c-gpio-0 conflict.
- `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md` — BD37033 investigation that
  led to all of this.
