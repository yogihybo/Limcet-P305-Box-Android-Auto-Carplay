# pin-force

Directly forces one of the ARK1668 LCD RGB888 data pins (`r0`-`r7`,
pins 2-9) into GPIO mode at a specific level, or restores it to LCD
function, via raw physical register writes -- bypassing the kernel
entirely.

## Why

A deliberate, active test to settle the tension found while
investigating the LCDTest color-grid corruption: a prior static
debugfs snapshot (`docs/DISPLAY_SUBSYSTEM.md`'s
`I2C_GPIO0_LCD_PIN_CONFLICT` section) found pins 2/3 permanently muxed
to LCD function even after `i2c-gpio-0` claims them as GPIO, concluding
the shared-pin conflict only breaks I2C, not the LCD. But real hardware
testing since then showed a color tint that visibly moves across the
screen in sync with I2C activity. If forcing a pin (e.g. `r7`, shared
with the BD37033 audio chip's I2C bus on pin 9) low or high produces a
predictable, visible change on the LCDTest grid, that's direct proof
the pad really is drivable this way -- despite appearing permanently
LCD-muxed in a single snapshot.

See `tools/pinmux-watch/` for the passive/observational counterpart
(watch for a live mux change during real I2C traffic, rather than
forcing one).

## Register reference

- **Pinmux**: `0xe4900000 + 0x1c0`, one 4-bit nibble per pin 2-9 (pin N
  -> bits `[4*(N-2)+3 : 4*(N-2)]`). `0` = GPIO function, `1` = LCD
  function (`ARK_PVAL_1`, `dt-bindings/pinctrl/ark-pinfunc.h`).
- **GPIO0 MOD** (direction): `0xe4600000 + 0x00`, bit N = pin N
  direction, clear = output.
- **GPIO0 RDATA** (level): `0xe4600000 + 0x04`, bit N = pin N output
  level in output mode.

Pin reference: `r0`=2 `r1`=3 `r2`=4 `r3`=5 `r4`=6 `r5`=7 `r6`=8 `r7`=9.
Pins 2/3 are `i2c-gpio-0`'s SCL/SDA (RN6752 camera decoder). Pin 9 is
`i2c-gpio-1`'s SDA (BD37033 audio chip) -- the higher-impact one to
test if `r7` really is the R channel's most-significant bit (128 of
255) rather than `r0`/`r1`'s two least-significant bits (at most
+/-3).

## Usage

```
pin-force <pin 2-9> <status|gpio-low|gpio-high|lcd>
```

- `status`: print the pin's current pinmux nibble + GPIO MOD/RDATA bit,
  without changing anything.
- `gpio-low` / `gpio-high`: force the pin to GPIO mode, set it to
  output, and drive it low/high.
- `lcd`: restore the pinmux nibble to `1` (LCD function). Does not
  touch GPIO_MOD/RDATA -- irrelevant once re-muxed away from GPIO.

Example test on `r7` (pin 9):

```sh
./pin-force 9 status                # confirm starting state (should read "LCD")
./pin-force 9 gpio-low               # force r7 low -- watch the LCDTest grid
# look for a predictable, uniform change (e.g. R channel dropping ~128
# across the whole screen if r7 is the R MSB)
./pin-force 9 gpio-high              # force r7 high -- watch again
./pin-force 9 lcd                    # restore to LCD function when done
./pin-force 9 status                 # confirm restored
```

## Safety note

This writes raw hardware registers on a live system, bypassing every
kernel driver's own bookkeeping (`pinctrl-ark.c`, `gpio-ark.c`,
`ark1668_lcdfb.c` all stay unaware of the change). Expected to be safe
and fully reversible -- a reboot restores the real boot-time pinmux
either way, and `lcd` mode restores the documented function value --
but this is a tool for live, deliberate, watched experiments only. Not
something to leave running or call from a script. Requires root (raw
`/dev/mem` read/write).
