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
| 5 | `detect` | unidentified — name suggests card/lid/door detect, not yet traced |
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
- [ ] Trace GPIO `5`'s `detect` consumer — unidentified so far.
- [ ] Revisit the GT911/rn6752/BD37033 real-wiring question with this
      corrected understanding once a safer verification method
      (schematic, or single-pin tests with the SD card backed up) is
      available.

## Cross-references

- `docs/I2C_GPIO0_LCD_PIN_CONFLICT.md` — the LCD/i2c-gpio-0 conflict.
- `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md` — BD37033 investigation that
  led to all of this.
