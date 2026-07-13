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

## Driver source reference (2026-07-14)

For every peripheral above/below: which driver claims it, where that driver's
source actually lives, and how solid that is. `linux-arkmicro` paths are
relative to `/home/osboxes/Downloads/linux-arkmicro/linux/`. **Checked
against the tree directly this session** (grepped for each DTS `compatible`
string against the actual driver's `of_match_table`, not assumed from
filename) — where two similarly-named files existed for one peripheral
(e.g. two carback drivers), the table lists the one whose compatible string
actually matches this project's DTS, not the other SoC variant.

**"Confirmed by operation" column — status key** (added 2026-07-14, cross-checked
against `docs/new kernel bootlog new uboot v11.txt`, the most recent full boot
log, plus dedicated docs where the boot log alone wasn't enough).

**Important caveat on the boot log itself, added after user pushback
2026-07-14: a clean dmesg probe line is weak evidence, not proof of correct
operation.** A driver can print a success message and register a device node
while the actual hardware behind it is silently misbehaving — wrong data,
no physical response, a downstream failure that never reaches the kernel
log at all. "No error in dmesg" only means the *driver's own init path*
didn't hit a failure condition it knows to check for; it says nothing about
whether the peripheral is functionally correct. Treat **CONFIRMED** as the
only status here that means "actually verified," and read **PROBES OK**
as "unknown, leaning slightly more hopeful than NOT CONFIRMED" — not as
"basically working."
- **CONFIRMED** — physically observed actually working (video visibly rendering,
  audio audibly playing, a real SD card/WiFi AP/USB device enumerating), not
  just "driver loaded without an error."
- **PROBES OK** — the driver initializes without a logged error, but this is
  **not evidence of correct operation** — no functional/physical test has
  confirmed the feature actually works, and a real problem could easily be
  invisible to dmesg. Treat as unverified, closer to NOT CONFIRMED than to
  CONFIRMED.
- **NOT CONFIRMED** — no success evidence found either way; don't assume it works.
- **DORMANT** — intentionally inactive on this hardware (nothing loads it, or
  the hardware isn't populated) — not a bug, just not in use.
- **N/A** — no kernel driver/operation concept applies.

| Peripheral | DTS `compatible` | Driver source | Confirmed by operation | Notes |
|---|---|---|---|---|
| LCD framebuffer | `arkmicro,ark1668-lcdc` | `drivers/video/fbdev/arkmicro/ark1668_lcdfb.c` | **CONFIRMED** | video physically observed on screen across many sessions; boot log: `ark1668_lcdfb e0500000.lcd: fb0: Atmel LCDC at 0xe0500000... irq 25` |
| `/dev/ark_display` misc ioctl shim | `ark_display` (misc device, not a DT node) | `Limcet Hardware/ark_display.c` | PROBES OK | boot log: `ark_display: registered /dev/ark_display` — confirms the shim loads and fixes the specific ioctl it targets; whether it also fully unblocks `MsnCoreApp` end-to-end wasn't separately re-verified this session |
| NAND | (`ark1668,nand`-style, see `ark1668.dtsi`) | `drivers/mtd/nand/raw/ark_nand.c` | **CONFIRMED** (boot chain) | full stock UI boots from NAND via the `bootstock` U-Boot path (project memory, 2026-07-13); this specific boot log predates/differs from that ECC fix and still shows `ark_nand_correct_data: uncorrectable ECC error` spam — don't take *this log* as proof the ECC fix is deployed everywhere, only that NAND boot itself is a proven-working path |
| UART0-3 | `arkmicro,ark-uart` | `drivers/tty/serial/ark_uart.c` | uart0: **CONFIRMED** (console); uart1-3: NOT CONFIRMED | uart0 is the serial console this entire investigation has been conducted over; uart1-3 have no evidence of any attached function being tested |
| hsuart uart4 (MCU link, `/dev/ttyHS0`) | `arkmicro,ark-hsuart` | `drivers/tty/serial/ark_hsuart.c` | PROBES OK | boot log: `e4f00000.serial: ttyHS0 at MMIO 0xe4f00000 (irq = 38...) is a ARK HS UART` — tty node exists and driver loads; no evidence in this session of actual MCU protocol traffic (key events, `recv track:`, etc.) confirmed flowing over it on this kernel build |
| hsuart uart5 (Bluetooth, `/dev/ttyHS1`) | `arkmicro,ark-hsuart` | `drivers/tty/serial/ark_hsuart.c` | **CONFIRMED** | boot log: `e4800000.serial: ttyHS1 at MMIO 0xe4800000 (irq = 44...) is a ARK HS UART`, plus `Bluetooth: HCI UART driver ver 2.3` loading; `wireless_and_init_documentation.md` documents this as an already-working, tested setup (RTL8762BTV/BT825) |
| HW I2C0 | `snps,designware-i2c` | `drivers/i2c/busses/i2c-designware-{master,platdrv}.c` | PROBES OK | bus itself comes up; the only device on it in the DTS (Goodix-TS) fails to ACK (`Goodix-TS 1-005d: I2C communication failure: -6` — expected/consistent with GT911 being dormant, not a bus fault) |
| i2c-gpio1/2/3 (bit-banged) | `i2c-gpio` | `drivers/i2c/busses/i2c-gpio.c` | PROBES OK | boot log confirms both buses register (`i2c-gpio i2c-gpio-0: using lines 3 (SDA) and 2 (SCL)...`, `i2c-gpio-1: using lines 9 (SDA) and 121 (SCL)...`) |
| rn6752 camera decoder | `arkmicro,ark1668_rn6752` | `drivers/soc/arkmicro/itu656/rn6752.c` | NOT CONFIRMED | only evidence in the latest boot log is the recurring `### rn6752_eq_work reset` line — no clear probe-success message found; actual camera video not confirmed working on this kernel build this session |
| BD37033 audio codec | `arkmicro,drv_bd37033` | `sound/soc/arkmicro/BD37033.c` | NOT CONFIRMED (codec control); I2S path itself CONFIRMED | no `bd37033` probe line found at all in the latest boot log (worth checking whether the module actually loaded on that boot); however basic I2S audio output has its own dedicated confirmed-working session (`new kernel audio working log v1.log`, `AUDIO_SUBSYSTEM_INVESTIGATION.md`'s dai-link fix) — so audio *plays*, but BD37033's own I2C-controlled volume/effects path status is unclear from this session's evidence |
| GT911 touch (dormant on this unit) | `goodix,gt911` | `drivers/input/touchscreen/goodix.c` | **DORMANT** (confirmed inactive, not a failure) | boot log's `I2C communication failure: -6` is the *expected* signature of this being unpopulated hardware, consistent with prior conclusions — not evidence of a bug |
| Resistive touch (actually used) | `arkmicro,ark1680-ts` | `drivers/input/touchscreen/ark1680_ts.c` | PROBES OK; physical touch input inconclusive | boot log: `input: ark1680-ts as .../input0`, `ARK1680 resistive touchscreen registered, irq=20` — driver probes cleanly; whether physical touches actually register as input events on *this* kernel build is not confirmed by this evidence alone (see `ARK1680_TS_REVERSE_ENGINEERING.md`'s mixed findings, largely gathered on stock firmware, not this kernel) |
| PWM1-3 | `arkmicro,ark-pwm` | `drivers/pwm/pwm-ark.c` | **CONFIRMED** (backlight) | screen visibly lit/backlight functional across every working-video session |
| I2S1 + DAC/ADC codecs | (ark1668 sound bindings) | `sound/soc/arkmicro/ark_i2s.c`, `ark1668-sddac-codec.c`, `ark1668-sdadc-codec.c`, `ark_dac_codec.c`, `ark_adc_codec.c` | **CONFIRMED** | dedicated working session/log (`new kernel audio working log v1.log`) plus the dai-link-order fix documented in `AUDIO_SUBSYSTEM_INVESTIGATION.md` |
| SPI | `arkmicro,ark-ecspi` (ARK1668, non-`e` variant) | `drivers/spi/spi-ark.c` | NOT CONFIRMED | no evidence any SPI-attached device has been tested |
| `ark_carback` (reverse-gear trigger, GPIO 5) | `arkmicro,ark-carback` | `drivers/soc/arkmicro/ark-carback.c` | NOT CONFIRMED | no `carback` probe line found in the latest boot log; GPIO 5 wiring itself is triple-confirmed (stock disasm, Ghidra, and this driver's own `devm_gpiod_get(pdev, "detect")` source), but an actual reverse-gear trigger test hasn't been evidenced this session |
| `ark_nec_sw_remote` (IR) | n/a | n/a | **N/A** | no IR sensor populated on this hardware revision — confirmed by the user, 2026-07-13 |
| GPIO 95 `apple_encpy_ic_rst` | none (no DT node) | none | **N/A** | stock-3.4-only board-init GPIO poke; not modeled in the 4.9 DTS at all, chip presence on this unit unconfirmed |
| GPIO 91 `BTEN` (Bluetooth enable) | none (plain sysfs) | none | **CONFIRMED** | `wireless_and_init_documentation.md` documents this as part of an already-working, tested Bluetooth setup |
| Bluetooth stack (RTL8762BTV/BT825 over `/dev/ttyHS1`) | n/a (userspace) | Feasycom/`rtkbt` userspace BT stack (rootfs, not kernel) | **CONFIRMED** | see uart5 row above |
| WiFi (RTL8811CU, USB) | n/a (USB autoload) | `drivers/net/wireless/realtek/rtl8811cu/` | **CONFIRMED** | user-confirmed working; boot log: `wlan0: interface state UNINITIALIZED->ENABLED`, `wlan0: AP-ENABLED`, SSID `carplay_wifi` |
| MMC0 (SD card slot) | `snps,dw-mshc` | `drivers/mmc/host/dw_mmc.c` | **CONFIRMED** | user-confirmed working; boot log: `mmc0: new SD card at address e126`, `mmcblk0: mmc0:e126 SU01G 969 MiB` |
| MMC1 | `snps,dw-mshc` | `drivers/mmc/host/dw_mmc.c` | NOT CONFIRMED / purpose unclear | boot log shows bus-speed negotiation only (`mmc_host mmc1: card is non-removable`), no explicit device bind message captured this session — DTS comments it as "SDIO WiFi Controller," but the WiFi that's actually confirmed working is the USB RTL8811CU, so `mmc1`'s real role on this unit needs re-checking, not assumed from the DTS comment |
| USB (MUSB) | `arkmicro,ark-musb` | `drivers/usb/musb/musb_ark.c` | **CONFIRMED** | user-confirmed working; boot log shows hub enumeration on both controllers and the WiFi dongle attached via USB |

**Net takeaway**: almost every peripheral already has real, in-tree driver
source in the buildable `linux-arkmicro` kernel tree — the stock 3.4
disassembly work in this doc was about recovering **wiring/pin ground
truth** (which the 4.9 *driver* code doesn't tell you), not about missing
driver code. The two genuine gaps are IR (hardware not populated, moot) and
GPIO 95's CarPlay/MFi chip (presence on this unit still unconfirmed).

**Second net takeaway, from the "confirmed by operation" column**: source
existing and a driver probing cleanly is a materially weaker claim than
something being *observed working*. Only WiFi, USB, MMC0, the LCD/backlight,
I2S audio, uart0 console, and Bluetooth are actually confirmed operating
end-to-end. Several other peripherals with clean driver source and a
successful probe in the boot log — the MCU link (`ttyHS0`), carback, rn6752,
BD37033's own I2C control path, and resistive touch's actual finger-touch
behavior — have **not** been confirmed to functionally work on this exact
4.19 kernel build this session, and shouldn't be assumed to just because
their driver loads without an error. Worth prioritizing an actual functional
test for these before treating them as "done."

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
| 91 | `BTEN`/`gpio91` | **Bluetooth module enable pin** (RTL8762BTV/BT825) — confirmed working, `docs/wireless_and_init_documentation.md` §1,4 (`BTEN_INTERFACE=gpio91` in `/etc/blueware-bw121.properties`, toggled via plain `/sys/class/gpio/gpio91` export/direction/value). **Falls inside the previously-flagged 85-96 "unverified" range — now resolved for this one pin**: 91 is a real, safe, in-use GPIO, not unknown/unsafe. |
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

**`uart5` (hsuart port 1, `/dev/ttyHS1`) confirmed — this is the Bluetooth UART, not a spare.**
Originally flagged here only as "very likely" Bluetooth from stock rootfs config
(`blueware-bw121.properties`/`rtkbt.conf` referencing `/dev/ttyHS1`). That's now
upgraded to confirmed: `docs/wireless_and_init_documentation.md` independently
documents this exact interface **already working** on a real build (Realtek
RTL8762BTV/BT825 module, `UART_INTERFACE=/dev/ttyHS1`, `UART_BAUDRATE=1500000`),
plus a GPIO enable pin (**GPIO 91**, `BTEN_INTERFACE=gpio91`, toggled via plain
`/sys/class/gpio/gpio91` export — no DTS `gpio-hog`/consumer node needed, it's
driven entirely from userspace/the Feasycom daemon). So the full picture for
`uart5`/`ttyHS1` is now: MMIO `0xe4800000`/IRQ 28 (Ghidra/board-file, this
session) + `compatible = "arkmicro,ark-hsuart"` (4.9 DTS, matches exactly) +
GPIO 91 enable (`wireless_and_init_documentation.md`, independently confirmed
working) — all three sources agree, and Bluetooth is a fully accounted-for
peripheral, not an open item. GPIO 91 also **resolves one pin out of the
"unverified 85-96" unsafe range** below — see the GPIO table above.

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

- `docs/PIN_BLOCK_DIAGRAM.txt` — plain-text block diagram of every
  peripheral/pin/GPIO in this doc, for a quick at-a-glance view. Keep both
  files in sync when either changes.
- `docs/I2C_GPIO0_LCD_PIN_CONFLICT.md` — the LCD/i2c-gpio-0 conflict.
- `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md` — BD37033 investigation that
  led to all of this.
