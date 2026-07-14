# pin-dump.sh

Dumps every ARK1668 pinmux pad's **live hardware mux value**, read directly via
`devmem` — bypassing the Linux pinctrl subsystem entirely.

## Why

Our reconstructed kernel's `pinctrl-ark.c` exposes
`/sys/kernel/debug/pinctrl/*/pinmux-pins`, which shows each pin's *software*
claim (which driver currently thinks it owns the pin). That's useful on our
own image, but meaningless on **stock firmware** — the stock 3.4 kernel uses
legacy board-file GPIO/platform-device init, not devicetree/pinctrl, so it has
no debugfs pinctrl interface at all (see
`docs/I2C_GPIO0_LCD_PIN_CONFLICT.md`).

`devmem` reads the raw physical register directly. The register layout is
fixed by the SoC silicon, not by which kernel/driver model happens to be
running — so this script produces directly comparable output on **both**
stock and our own reconstructed image. That comparison is the whole point:
e.g. "does stock's real, working-in-the-field firmware leave pin 9 muxed to
the LCD too, or does it switch it to something else?"

Stock's own busybox already has `devmem` built in (confirmed via `strings` on
the dumped stock `busybox`, v1.25.0) — no payload/binary transplant needed to
run this on stock, just get a root shell (e.g. via `msn_autocopy/`) and copy
this one script over.

## Where the register table comes from

`drivers/pinctrl/pinctrl-ark.c`'s `ark1668_pin_map[]` — the exact table the
`"arkmicro,ark1668-pinctrl"` compatible string (this board's pinctrl driver)
uses for `pin index -> {reg, offset, mask}`. Array index N == pin N in
`pinmux-pins`' own numbering — confirmed against
`I2C_GPIO0_LCD_PIN_CONFLICT.md`'s independently-verified live `pinmux-pins`
reads for pins 2/3 (LCD r0/r1) and this investigation's own read of pins 9/121.

All registers are 32-bit, at `0xe4900000 + reg`. Each pin's raw mux value is
`(regval >> offset) & mask` — compare against `ARK_PVAL_0`..`ARK_PVAL_7` in
`include/dt-bindings/pinctrl/ark-pinfunc.h`. This script prints the raw
number, not a decoded function name, since the available function set differs
per pin and isn't captured in this table alone (cross-reference
`ark1668-pinctrl.dtsi` / `ark1668e-pinctrl.dtsi` for what a given `ARK_PVAL_n`
means on a specific pin).

## Usage

```sh
# dump all 131 known pins
./pin-dump.sh

# dump specific pins only
./pin-dump.sh 2 3 9 121
```

## Decoding raw PVAL numbers into function names

`pin-dump.sh` only prints raw `PVAL` numbers — it has no notion of what a
given value *means* for a given pin. `decode-pins.py` (run on a workstation,
not the device) cross-references a capture against every named `ark,pins`
group in `ark1668-pinctrl.dtsi`, reconstructing the same "function X group Y"
info the live debugfs `pinmux-pins` route gives on our own board — which
matters most on stock, since it has no such interface at all.

```sh
python3 decode-pins.py \
  /path/to/linux-arkmicro/linux/arch/arm/boot/dts/ark1668-pinctrl.dtsi \
  captured-pin-dump.txt
```

Output per pin is one of:
- `<comment> (group <name>)` — a recognized function, e.g. `r7 (group lcd-rgb-0)`.
- `UNRECOGNIZED PVAL=n (known options: ...)` — the pin has named functions
  in the dtsi, but not at the value actually read (worth investigating).
- `no named pinctrl group (...)` — no `ark,pins` entry claims this pin at
  all in this dtsi; likely plain GPIO, or a board-specific fixed-purpose
  pin not exposed as a named mux group (see `KNOWN_EXTRA` in the script for
  the handful of those already identified, e.g. `i2c-gpio-1`'s SDA/SCL).

## Sanity-checked, and run on both boards

Verified locally against a stub `devmem` before running on real hardware:
pin 9 with a synthetic register value encoding `ARK_PVAL_1` at bits `[31:28]`
correctly decodes to `PVAL=1` — matches the debugfs `pinmux-pins` value
independently confirmed live (`lcd-rgb-0`). Full 131-pin dump and targeted
pin subsets both run without error.

Run for real (2026-07-14) on both our reconstructed image and real stock
hardware (copied over via the existing `msn_autocopy` root-shell method,
no transplant needed since stock's own busybox already has `devmem`) —
both show pin 9 (`reg=0x1c0 offset=28 mask=0xf`) as `PVAL=1`
(LCD-muxed), byte-for-byte identical. See
`docs/AUDIO_SUBSYSTEM_INVESTIGATION.md`'s "Live hardware re-test" section
and `docs/pindump stock.txt` for the full comparison and what it does/
doesn't prove about the BD37033 I2C write-timeout investigation.
