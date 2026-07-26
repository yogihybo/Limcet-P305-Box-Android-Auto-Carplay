# lcdc-regdump

Dumps every named ARK1668 LCDC register (by name, not raw offset) to
stdout, so a stock-vs-our-build run can be diffed directly. Built to
finally settle the deterministic `LCDTest -qws` black-cell color bug
empirically, after static/source comparison (`rgb_order`,
`rgb_ycbcr_bypass`, colorkey/threshold, blend mode, gamma, sibling-board
drivers) was exhausted without finding the cause.

Stock rootfs is known-good (correct colors, alpha, etc.); ours isn't.
Since we can't inspect stock's kernel source, the fastest way to find
what's actually different is to dump every LCDC register on both while
they're showing the *same* test pattern, and diff the text output --
whatever differs (that isn't flagged `(dynamic)`) is the lead.

## How it works

Reads via `mmap()`'d `/dev/mem` at the real physical `LCD_BASE`
(`0xE0500000`, from U-Boot's `ark1668_hardware.h`) -- the same access
method `mem-dump` already uses successfully on this hardware. The
register table (242 entries, offsets `0x00`-`0x3fc`) is mechanically
extracted from `linux/include/linux/soc/arkmicro/ark1668_lcdc_regs.h`
(see `lcdc_regtable.inc`), not retyped by hand, so it can't drift from
the real header. A few offsets have two names (e.g. `EN`/`DMABADDR1`
both at `0x00`) because the vendor header itself aliases them
depending on context -- both are printed, same value.

Palette RAM (`Palette_BASE = 0x400` onward) is deliberately **not**
dumped -- `LCDTest` uses RGBA888/RGB888 (non-palette) formats. Add it
back later if a palette-format investigation ever needs it.

## Usage

Run on **both** systems while `LCDTest -qws` is showing the identical
test pattern/page (same screen, not just the same app):

```sh
# on stock:
lcdc-regdump stock > /data/regs_stock.txt

# on our build:
lcdc-regdump ours > /data/regs_ours.txt
```

Then diff the two (copy both files off-device however you've been
moving files, e.g. via SD card or network share):

```sh
diff regs_stock.txt regs_ours.txt | grep -v '(dynamic)'
```

Every line that differs and isn't marked `(dynamic)` is a concrete,
confirmed hardware-state discrepancy -- cross-reference the register
name against `ark1668_lcdc_regs.h` / the driver code that writes it to
find where our init sequence diverges from stock's.

## Known-dynamic registers (expected to differ, not a lead)

Flagged inline with `(dynamic)`: frame-start counters
(`TIMING_FRAME_START_CNT_LCD`/`_TV`), `INT_STATUS` (live IRQ flags),
and video2 write-back progress (`WB_DATA_PER_FRAME_VIDEO2`,
`WRITE_BACK_ADDR_VIDEO2`). These vary run-to-run regardless of the bug.
If a genuinely static-looking register (OSD/blend/colorkey/format
config) shows up as differing, that's real signal -- don't dismiss it
even if it "seems" like it could be dynamic; only the four named above
are expected to vary.

## Caveats

- **Requires the binary to actually run on stock rootfs**, which is a
  separate filesystem from our build -- get it onto stock however
  static tools have been copied over there before (SD card, USB, etc.);
  this tool doesn't handle that part.
- Both runs need to be on the *same physical panel state* -- same
  `LCDTest` page, same resolution/orientation, ideally soon after boot
  on both sides so nothing else has had a chance to touch OSD/video
  layer registers first.
- Statically linked, no libc/NSS dependencies -- doesn't need the
  `tools/nss-stub` treatment other static tools sometimes require.
