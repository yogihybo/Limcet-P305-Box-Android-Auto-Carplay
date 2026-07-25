# fb-scan

Locates every solid-color rectangle in the live `/dev/fb0` framebuffer,
and for each near-black one, predicts what color it should actually be
by fitting a line through the other solid cells in the same grid row.
Written to pin down the exact coordinates of the two black cells in
`LCDTest -qws`'s red intensity-ramp row without guessing offsets from a
photo, and to tell app-write-bug from display-pipeline-bug apart without
re-deriving LCDTest's runtime QRect layout math by hand.

Uses `mmap()` on `/dev/fb0` itself, not `/dev/mem` -- fbdev's own mmap
works fine for its own memory (unlike DMA carve-out regions elsewhere in
the display pipeline, see `tools/mem-dump/`), so no special handling is
needed here.

The expected-color prediction is deliberately self-referential: it fits
a line through the *other* cells actually rendered in the same row on
this run, rather than hardcoding LCDTest's pixel geometry from its
disassembly (that layout is computed at runtime from the actual widget
size via several chained float ops -- fragile to reproduce exactly). One
piece of static ground truth is used as a sanity check only: LCDTest's
ramp rows are confirmed (paintEvent disassembly, ~0x13398-0x133b4) to
step in exactly `floor(k*255/10)` for k=0,2,4,6,8, with k=10 forced to
255 -- six discrete stops `{0,51,102,153,204,255}`. If a prediction
lands far from that set, be suspicious of the prediction itself, not
just the hardware.

## Usage

```
fb-scan [threshold]
```

`threshold` (default 4): max per-channel R/G/B value still counted as
"black", and also the tolerance used when grouping pixels into a solid-
color rectangle. Run it while `LCDTest -qws` is on screen showing the
bug:

```
QWS_DISPLAY=directfb:... LCDTest -qws &
fb-scan
```

Output, one line per detected rectangle >= 25px²:

```
COLOR x=<x> y=<y> w=<w> h=<h> px=0x<raw pixel> r=<r> g=<g> b=<b>
BLACK x=<x> y=<y> w=<w> h=<h> px=0x<raw pixel>
  EXPECTED r=<r> g=<g> b=<b> (linear fit from N cell(s) in same row, dominant=R/G/B)
```

or, if fewer than two color cells share the black cell's row:

```
  no row fit available: only <N> color cell(s) found in this row
```

Expect one large `COLOR` (or near-black `BLACK`) rectangle for the app's
background plus, if the bug is present, `BLACK` entries inside the
red-ramp row's cell band with an `EXPECTED` line predicting what they
should look like. If the raw `px` really is `0x00000000` in the DMA
memory (cross-check with `mem-dump` at the same coordinates translated
to the physical framebuffer address) while `EXPECTED` predicts a
plausible non-black value, that points at the display pipeline (LCDC
register/compositor) discarding real data; if `mem-dump` shows the
*same* nonzero-but-wrong bytes LCDTest itself wrote, that points at an
app/library-level write bug instead.
