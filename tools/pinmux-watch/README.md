# pinmux-watch

Tight-loop poller for the ARK1668 pad-mux control registers covering
the LCD RGB888 data pins, to catch a live function-select change (GPIO
vs LCD) in the act, correlated with I2C bus activity on the pins shared
with it.

## Why

`docs/DISPLAY_SUBSYSTEM.md`'s `I2C_GPIO0_LCD_PIN_CONFLICT` section
found -- via a single debugfs snapshot of
`/sys/kernel/debug/pinctrl/e4900000.pinctrl/pinmux-pins` -- that pins
2/3 (LCD `r0`/`r1`, shared with `i2c-gpio-0`'s SCL/SDA for the RN6752
camera decoder) stay muxed to LCD function even after `i2c-gpio-0`
successfully `gpio_request()`s them, and concluded the conflict only
breaks I2C (pure software bookkeeping, no real electrical effect on the
pad) -- not the LCD.

But that was one static snapshot taken at an arbitrary idle moment, not
during active I2C bit-toggling. Since then, real hardware testing
showed a color tint that visibly **moves across the screen in sync with
I2C message activity** -- which only makes sense if the pad's actual
drive source really does change, at least momentarily, during a
transaction. This tool settles it empirically: poll the exact register
as fast as possible and log every value change with a monotonic
timestamp, so a change can be correlated against I2C traffic triggered
in another terminal.

## Register layout

From `drivers/pinctrl/pinctrl-ark.c`'s `ark1668_pin_map[]` (indexed by
global pin number): pins 2 (`r0`) through 9 (`r7`) are all packed into
**one 32-bit register** at `pinctrl0`'s base (`0xe4900000`, from
`ark1668.dtsi`) `+ 0x1c0`, one 4-bit nibble per pin, LSB-first (pin2 =
bits[3:0], pin3 = bits[7:4], ..., pin9 = bits[31:28]).
`ark_gpio_request_enable()` clears a pin's nibble to `0` for GPIO
function; nonzero means some peripheral function is selected. `g0`-`g7`
(pins 10-17, `+0x1c4`) and `b0`-`b7` (pins 18-25, `+0x1c8`) are packed
the same way and watched too, in case interference isn't limited to the
R channel.

**Pin 9 (`r7`) is also `i2c-gpio-1`'s SDA line (BD37033 audio chip
bus)** -- a second, independent conflict beyond the already-documented
pin2/3 one, and much higher-impact if `r7` is the R channel's
most-significant bit (128 of 255) rather than `r0`/`r1`'s two
least-significant bits (at most +/-3).

## Usage

```
pinmux-watch [duration_sec]
```

`duration_sec` (default 30): how long to poll. Trigger I2C activity
(adjust volume to hit BD37033, wait for an `rn6752_eq_work` retry,
etc.) in another terminal/session while this runs.

Output, one line per detected change:

```
[<seq>] t=+<usec> RGB_0_7=0x<val> (was 0x<val>) GRN_0_7=... BLU_0_7=...
```

A change in `RGB_0_7` during I2C activity -- especially in the nibble
matching whichever pin is mid-transaction -- confirms the pad's
function-select really does flip live, settling the tension between the
prior static-snapshot finding and the observed moving color tint.
