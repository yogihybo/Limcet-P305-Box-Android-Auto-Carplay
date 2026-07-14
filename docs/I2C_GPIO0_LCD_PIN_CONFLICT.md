# `i2c-gpio-0` / LCD RGB888 pin conflict (found 2026-07-13, live serial console)

## Summary

`i2c-gpio-0` (SDA=GPIO3, SCL=GPIO2 on `gpiochip0`) sits on the **exact
same physical SoC pins** as the LCD controller's RGB888 parallel data
bus, bits R1/R0. The LCD is actively using those pins right now
(RGB888 mode is what's selected and the screen works). This is very
likely the root cause of *every* unexplained I2C communication failure
seen on this bus across the whole project: GT911 touch never ACKing
(`docs/boot_experiment_log.md`), and now BD37033 audio register writes
timing out (`docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`). The `rn6752`
camera decoder also lives on this bus and has needed a manual reset
workaround (`### rn6752_eq_work reset` in every boot log) that was never
fully explained either.

## How this was found

While probing the BD37033 I2C write-timeout issue live over serial
(see `AUDIO_SUBSYSTEM_INVESTIGATION.md`'s "Live re-test" section), we
mounted debugfs and checked `/sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins`
to see what the pin-mux subsystem thinks pin 2/pin 3 are configured
for — expecting to confirm the DTS's intended GPIO/I2C assignment.
Instead:

```
# cat /sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins | grep -E "pin 2 |pin 3 "
pin 2 (pin2): e0500000.lcd e4600000.gpio:2 function lcd group lcd-rgb-0
pin 3 (pin3): e0500000.lcd e4600000.gpio:3 function lcd group lcd-rgb-0
```

The pinctrl subsystem's own live mux table says pins 2 and 3 are muxed
to the **LCD function**, group `lcd-rgb-0` — not to plain GPIO.

## Confirming the mapping is real, not a debugfs labeling quirk

1. **`i2c-gpio-0`'s GPIO assignment** (`Limcet Hardware/ark1668-limcet-prado.dts:44-52`):
   ```c
   i2c-gpio-0 {
       compatible = "i2c-gpio";
       gpios = <&gpio0 3 0   /* SDA (GPIO 3) */
                &gpio0 2 0   /* SCL (GPIO 2) */>;
       i2c-gpio,delay-us = <6>;
       i2c-gpio,scl-output-only;
       ...
   };
   ```
   `&gpio0` is `gpiochip0` (`e4600000.gpio`).

2. **`gpiochip0`'s pin-range mapping** (live, `/sys/kernel/debug/pinctrl/e4900000.pinctrl/gpio-ranges`):
   ```
   0: e4600000.gpio GPIOS [0 - 5] PINS [0 - 5]
   ```
   GPIOs 0-5 on `gpiochip0` map 1:1 to global pinctrl pins 0-5. So
   `i2c-gpio-0`'s GPIO2/GPIO3 *are* global pinctrl pins 2 and 3.

3. **Global pin numbering formula** (`pinctrl-ark.c:25,773` in the
   buildable kernel tree, `/home/osboxes/Downloads/linux-arkmicro/linux/drivers/pinctrl/pinctrl-ark.c`):
   ```c
   #define MAX_PIN_PER_BANK 32
   grp->pins[i] = pin->bank * MAX_PIN_PER_BANK + pin->pin;
   ```
   and `ARK_PBANK_0 = 0` (`include/dt-bindings/pinctrl/*.h`). So global
   pin 2/3 == `ARK_PBANK_0` offset 2/3.

4. **What `ARK_PBANK_0` offset 2/3 is used for** (`ark1668-pinctrl.dtsi:45-70`,
   the `pinctrl_lcd_rgb888` / `lcd-rgb-0` group):
   ```c
   pinctrl_lcd_rgb888: lcd-rgb-0 {
       ark,pins =
           <ARK_PBANK_0 2 ARK_PVAL_1   /* r0 */
            ARK_PBANK_0 3 ARK_PVAL_1   /* r1 */
            ...
   ```
   `ARK_PBANK_0` offset 2 is LCD **r0**, offset 3 is LCD **r1** — the
   two least-significant bits of the red channel in RGB888 mode.

5. **RGB888 mode is what's actually selected** (`ark1668-limcet-prado.dts:165`):
   ```c
   pinctrl-0 = <&pinctrl_lcd_base &pinctrl_lcd_rgb888>;
   ```
   Confirmed live: the LCD works (bootlogo, UI all render), so this
   pin group is genuinely active, not a dormant alternative.

All four independent lookups agree: `i2c-gpio-0`'s SCL/SDA and the
LCD's r0/r1 data lines are the same physical pad.

## Why this didn't immediately break everything visibly

The LCD peripheral almost certainly doesn't go through the generic
Linux GPIO/pinctrl request-and-lock framework for its RGB data pins —
it just applies the pin mux once at probe time (`pinctrl-0 = ...`)
and then drives the pins directly via its own hardware IP, bypassing
`gpio_request()`. That's why `i2c-gpio-0`'s `gpio_request()` for the
same pin numbers **succeeds without any conflict error** (confirmed
live via `/sys/kernel/debug/gpio` — `gpio-2`/`gpio-3` show up cleanly
owned by `i2c-gpio-0`, no `-EBUSY`). The GPIO framework's software
model is fully unaware that the pin's actual mux register still routes
the pad to the LCD.

**Net effect:** `i2c-gpio-0`'s bit-banged SCL/SDA toggles are pure
software bookkeeping — the pin's mux is stuck on "LCD r0/r1", so the
pad only ever outputs whatever the current pixel's red LSBs are (i.e.
noise, from I2C's perspective, changing every pixel clock). Any device
on this bus — GT911, rn6752, BD37033 — is trying to talk over a line
that's actually carrying video data, not I2C waveforms. This explains:

- GT911 never ACKing at `0x5d` on any bus scan
  (`docs/boot_experiment_log.md`'s exhaustive, previously-inconclusive
  investigation).
- The `rn6752_eq_work reset` workaround needed on every boot.
- BD37033's `bd37033_write_byte timeout` on every single register
  write (`docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`).

It is **not** a clock-stretching problem (the `i2c-gpio,scl-output-only`
DTS property is a red herring here) — the bus was never electrically
functional in the first place.

## Vendor cross-reference complicates the picture (2026-07-13, `/home/osboxes/Downloads/linux-arkmicro`)

Checked the vendor's other reference board DTS files
(`linux/arch/arm/boot/dts/tyw-dashboard.dts`/`.dtsi`), a real shipped
car-dashboard product using the same SoC. It independently confirms two
things found via stock-kernel disassembly: GT911 touch on `&i2c0`
(hardware), and its own camera decoder (`dvr_ark7116`, playing the same
role as our `rn6752`) on `i2c-gpio-0` with the **identical** `gpios =
<&gpio0 3 0 /* SDA */ &gpio0 2 0 /* SCL */>` — same physical pins 2/3.

At first this looked like it would resolve the conflict cleanly:
`tyw-dashboard.dts:86` selects `pinctrl-0 = <&pinctrl_lcd_base
&pinctrl_lcd_lvds>` — **LVDS mode, not RGB888** — so if LVDS used
entirely different pins, tyw-dashboard's `i2c-gpio-0` would be
conflict-free where ours isn't (RGB888 selected because that's what
this unit's actual panel needs, confirmed by working video).

**But it doesn't resolve that cleanly** — `pinctrl_lcd_lvds` (the same
`ark1668-pinctrl.dtsi:73-86` this project's DTS also defines, unused)
*also* claims `PBANK_0` offset 2/3, labeled `lvds dn`/`lvds dp` (mux
value `ARK_PVAL_0`, vs. RGB888's `ARK_PVAL_1` on the same pins). So
`tyw-dashboard`'s own shipped LCD configuration nominally conflicts
with its own `i2c-gpio-0` too, just via a different LCD sub-mode. That
board presumably works in production, which means one of:

1. This isn't a real electrical conflict at all — the LVDS group has an
   unusual extra `group-mux = <0x1e0 31 1 1>` property (unique to this
   group, not present on RGB888/hi-impedance), which may reroute `dn`/
   `dp` through a genuinely separate hardware path despite the
   `ark,pins` table listing the same pin numbers — i.e. the per-pin
   `ark,pins` entries for LVDS may be template boilerplate that doesn't
   reflect real electrical routing.
2. It's the same "stale board-file entry not matching populated
   hardware" pattern already found repeatedly in *this* project (GT911
   bus placement, BD37033 chip presence/address) — just also present in
   the vendor's own tyw-dashboard file, and it happens not to matter
   there because those specific I2C devices/addresses aren't really
   what's populated or exercised on shipping units.

**This does not override the live, runtime evidence from our own
board** — `pinmux-pins` debugfs directly reported pins 2/3 as owned by
`e0500000.lcd`, function `lcd`, group `lcd-rgb-0`, captured *during
actual working video output* on this exact unit. That's live state, not
a static-DTS inference, and stands regardless of what another board's
DTS claims. But it's reason to hold the LCD-conflict theory with
somewhat less certainty than before — the vendor's own design doesn't
treat this pin overlap as automatically fatal, so there may be a
subtlety about which specific transactions/timing actually collide
(e.g. maybe brief, infrequent I2C bursts survive fine against a
constantly-toggling video signal often enough for the vendor's use case,
or the "conflict" only bites specific transaction patterns) rather than
a hard "the bus can never work at all" conclusion.

## Ground truth from real stock hardware (2026-07-13, live telnet + i2c-scan)

Got a root shell on **real stock firmware, real hardware** (via the `msn_autocopy` telnetd payload,
see `msn_autocopy/README.md`) and ran `tools/i2c-scan` directly against stock's own `/dev/i2c-*`
nodes — the actual, authoritative answer to the whole GT911/rn6752/BD37033 bus-placement question
this doc and `AUDIO_SUBSYSTEM_INVESTIGATION.md` have been chasing all session:

```
/sys/class/i2c-dev/i2c-0/name: ArkMicro I2C adapter      (hardware i2c0)
/sys/class/i2c-dev/i2c-1/name: i2c-gpio1
/sys/class/i2c-dev/i2c-2/name: i2c-gpio2

i2c-0: 0x10, 0x11 ACK  (same unidentified devices seen on our own board's hw i2c0)
i2c-1: 0x2c XX         (bound — rn6752 camera decoder)
i2c-2: 0x41 XX         (bound — BD37033 audio codec)

0x5d (GT911) — not present as XX or ACK on ANY of the three buses.
```

**This resolves the bus-placement question definitively, and it contradicts this project's
earlier "correction":**

- Stock's real, populated bus for **BD37033 is bus "2" (`i2c-gpio2`) — the original disassembly-derived
  `sda_pin=9, scl_pin=121` pins**, matching what this project's DTS had *before* an earlier session
  moved BD37033 onto `i2c-gpio-0` (GPIO2/GPIO3) based on the `XX`-marker misreading corrected
  earlier in this doc. **That earlier "correction" was wrong** — it moved BD37033 off its real bus
  and onto the LCD-conflicted one, which is very likely the actual root cause of the write-timeout
  failures found in `AUDIO_SUBSYSTEM_INVESTIGATION.md`.
- **rn6752 really is on bus "1" (GPIO2/GPIO3)** — the same pins implicated in the LCD RGB888
  conflict. It shows as bound (`XX`) on real stock hardware, consistent with it being a real,
  populated device that mostly works despite the pin sharing (matching the recurring
  `### rn6752_eq_work reset` workaround seen in every boot log — a plausible symptom of unreliable
  I2C on a contended bus, not proof the bus is totally non-functional).
- **GT911 is not present/responding anywhere on this physical unit, including stock's own intended
  bus (hardware `i2c0`, where stock's own board file registers it).** This is strong, hardware-level
  confirmation of the physical-population hypothesis raised early in `boot_experiment_log.md` — touch
  failure on this unit is very likely a real hardware issue (panel not wired/populated), not a
  software, DTS, or pin-conflict problem at all. No further software-side touch bus work is likely
  to fix it.

**Immediate action implied:** move BD37033 back to `sda_pin=9, scl_pin=121` (this project's own
`i2c-gpio-1`, before it was removed) — off the LCD-conflicted pins entirely — and leave rn6752 on
GPIO2/GPIO3 (`i2c-gpio-0`) since that's confirmed to be its real, populated bus regardless of the
LCD sharing.

**Correction (same session, later): the BD37033 write-timeout symptom itself is NOT unique to our
board's wrong-bus mistake.** A fresh stock boot dmesg (`dmesg` right after reboot, before the ring
buffer could wrap) shows the *identical* `bd37033_write_byte timeout` errors on real stock
hardware, on BD37033's own correct, factory-wired bus (`i2c-gpio2`, right at `[0.320000]
bd37033_drv_probe`, immediately followed by 8 write-timeout lines). So moving BD37033 back to its
real bus is still the architecturally correct fix (matches actual wiring), but it will not by
itself eliminate these specific timeout log lines — they appear to be an inherent, apparently-benign
quirk of this exact chip/driver/timing combination (most likely the driver's very first probe-time
write racing the codec's own power-up sequence), present in the vendor's own shipped firmware. The
`sendSoundData()` crash this investigation was chasing is a separate, distinct bug (see
`AUDIO_SUBSYSTEM_INVESTIGATION.md`), not explained by these timeouts.

## GT911 touch — resolved conclusively, real hardware fault

Also resolved live, same session: `/sys/bus/i2c/devices/0-005d` shows a `Goodix-TS` client
registered (the stock board file unconditionally declares it via `ark1680_add_device_i2c()`), but
`i2c-scan` shows `0x5d` completely silent, not even busy — inconsistent with `rn6752`/`drv_bd37033`,
which both correctly show `XX` matching their sysfs entries. Fresh boot dmesg resolves this: this
unit's `rcS` checks the `/msnprofile/ark1680_ts` marker file and only ever `insmod`s
`ark1680_ts.ko` (the resistive ADC touch driver) — `gt9xx.ko` (the driver that would actually bind
to and probe the declared Goodix-TS client) is **never loaded** on this unit. No driver bound means
no probe ever ran, which is why the address shows free rather than busy. Confirmed:
`ark1680_ts_probe` succeeds (`request_irq:4 success`).

**CORRECTION — the panel does work; IRQ count was the wrong signal to trust.** `/proc/interrupts`
showed IRQ 4 (`ark1680-ts`) at 0 both at rest and immediately after a physical touch test — which
looked like conclusive proof of a hardware fault, and was initially written up as such in this doc.
It was wrong: the user then directly interacted with the on-screen UI on this exact unit and it
worked. IRQ 4 stayed at 0 throughout, even during confirmed-working real touch interaction.

**Corrected understanding:** `ark1680_ts` evidently does not depend on that interrupt firing for
real touch sampling — most likely it's polling-based (a kernel timer/workqueue reading the ADC
registers periodically) with the IRQ serving some secondary/wake-only role, or none at all in
practice. IRQ activity count is **not a reliable signal for this driver** — don't use it again to
judge whether touch hardware is responding, on this unit or in future testing.

**This has a real implication for this project's own 4.19 kernel port.**
`ARK1680_TS_REVERSE_ENGINEERING.md`'s conclusion that our reconstructed driver's raw-ADC/IRQ
silence (`irq_status` pinned at 0, no conversions) meant a physical panel fault should be
revisited — if stock's own genuinely-working driver shows the identical IRQ silence, our driver's
problem may be a **fixable software issue** (e.g. missing the polling logic stock's driver
actually relies on, if ours only implemented the interrupt-driven path from the disassembly) rather
than proof the hardware is broken. Worth re-examining the stock driver's actual sampling mechanism
(polling vs. interrupt) before writing off touch on the custom kernel as a hardware dead end.

## `i2c-gpio-1` has the identical conflict (found 2026-07-14)

The "find pins that are actually free" item below was never checked
against `i2c-gpio-1` (GPIO9/GPIO121, used for BD37033) until now. It
has the same problem as `i2c-gpio-0`:

```
# cat /sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins | grep -E "pin 9 |pin 121 "
pin 9 (pin9): e0500000.lcd (GPIO UNCLAIMED) function lcd group lcd-rgb-0
pin 121 (pin121): (MUX UNCLAIMED) e4600060.gpio:121
```

`GPIO9` — `i2c-gpio-1`'s **SDA**, the pin carrying BD37033 traffic — is
muxed to the LCD's RGB888 group, same as pins 2/3. `GPIO_owner` shows
`UNCLAIMED` here (vs. an explicit owner for pins 2/3), meaning the
`i2c-gpio` driver never got hardware-level control of the pin at all.
`SCL` (pin 121) is unaffected, cleanly claimed as plain GPIO.

This fully explains `bd37033_write_byte timeout` on every image tested
throughout `AUDIO_SUBSYSTEM_INVESTIGATION.md` — including the fixed
`reg = <0x40>` address (see that doc's 2026-07-14 entries) — the SDA
line was never actually carrying I2C waveforms in the first place,
regardless of address. It also means the "probe-time race with the
codec's power-up sequence, apparently benign" theory recorded there
was formed without this pinmux check and should be treated as
superseded by this finding as the real explanation, not an independent
confirmation.

**Open puzzle:** the vendor's own real audio-control path (see
`AUDIO_SUBSYSTEM_INVESTIGATION.md`'s `arki2c_open` disassembly) also
uses `bus=2` = `i2c-gpio2` = these exact same GPIO9/121 pins (confirmed
above via stock's own `/sys/class/i2c-dev/i2c-2/name: i2c-gpio2`), and
presumably works in the field. Candidate explanations, not yet
distinguished: (a) the LCD only drives that bit during active display,
leaving real GPIO windows during blanking that a slow bit-banged
transaction could land in by chance or by design; or (b) matching the
pattern already found twice in this project (GT911 bus, BD37033
address), `lcd-rgb-0`'s claim on pin 9 may be a stale/overbroad
template inherited from a generic reference DTS that doesn't reflect
what this board's actual panel needs, freeing the pin electrically
despite the software-level mux claim.

## Open questions / next steps

- [ ] **Find pins that are actually free** for a real I2C bus on this
      board. Every other `PBANK_0`/`PBANK_2` assignment needs to be
      checked against the LCD RGB888 group (`r0`-`r7`, `g0`-`g7`,
      `b0`-`b7`, `de`, `clk`, `vsync`, `hsync` — `PBANK_0` offsets
      2-29) and the LCD base group (`PBANK_0` offsets 26-29) before
      reassigning `i2c-gpio-0`. **`i2c-gpio-1`/GPIO9 confirmed to have
      the same conflict, 2026-07-14 — see above.**
- [x] Check whether the **hardware `&i2c0`** controller uses pins free
      of this conflict — **confirmed clean, 2026-07-13 live check.**
      `&i2c0` (`e4300000.i2c`, DesignWare) uses `PBANK_2` offset 6/7
      (`ark1668-pinctrl.dtsi:204-210`, `pinctrl_i2c0`/`i2c0-0`) = global
      pins 70/71 — no other DTS group claims these pins, and live
      `pinmux-pins` confirms: `pin 70/71: e4300000.i2c (GPIO UNCLAIMED)
      function i2c0 group i2c0-0`, cleanly muxed with nothing
      contending. This also matches the earlier live scan
      (`boot_experiment_log.md`'s "Systematic I2C bus verification")
      that found real devices ACKing at `0x10`/`0x11` on this exact bus
      — i.e. it's not just clean at the mux level, something real is
      already answering on it. **This is the recommended target bus**
      for GT911/rn6752/BD37033 instead of hunting for new free GPIO
      pins to bit-bang — assuming the PCB wiring actually reaches these
      pins (still needs schematic/continuity confirmation, see below).
- [ ] Check the physical schematic (`Limcet Hardware/board_photo_*.jpg`
      if available, or any schematic PDF) for which pins these
      peripherals are actually wired to on the PCB — the pinmux
      conflict proves the *current DTS* is wrong, but doesn't by itself
      tell us which pins are correct. This needs either a schematic or
      continuity testing.
- [ ] Once a real candidate bus/pins is identified, re-test BD37033
      writes, GT911 ACK, and rn6752 stability all together — a single
      fix likely resolves three previously-separate-looking
      investigations at once.

## Cross-references

- `docs/AUDIO_SUBSYSTEM_INVESTIGATION.md` — BD37033 write-timeout
  investigation that led here.
- `docs/boot_experiment_log.md` — "Systematic I2C bus verification"
  section, the GT911 investigation this retroactively explains.
