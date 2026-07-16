# lcd-test

Live LCD/framebuffer diagnostic tool — same purpose/style as
`tools/i2c-scan/` and `tools/touch-test/`: a static ARM binary to
run at the live `/ #` root shell. Tests the raw kernel framebuffer
(`/dev/fb0`) directly, with no Qt/QWS server involved — so it works even
while `MsnCoreApp`/`LCDTest -qws` are segfaulting (see
`docs/ARK1680_TS_REVERSE_ENGINEERING.md` → "`MsnCoreApp` segfault").

## Build

Already built and checked in (static, stripped, armhf). Rebuild with:

```
arm-linux-gnueabihf-gcc -static -O2 -Wall -o lcd-test lcd-test.c
arm-linux-gnueabihf-strip lcd-test
```

## Usage

**Stop any running UI/compositor first** (`killall MsnCoreApp` — `rcS`
respawns it automatically). If a Qt/QWS process is still actively
repainting the screen, it will simply redraw over whatever `lcd-test`
just wrote within the next frame or two, making a real, successful write
look like nothing happened. This is the other likely explanation (besides
the panning bug noted below) if `fill`/`bars`/`gradient` report success
but the panel doesn't visibly change.

### `lcd-test` (or `lcd-test run` / `lcd-test test`)

Dumps the screen/framebuffer info to the console first, and then automatically cycles through all test patterns:
1. **Random Noise** (2 seconds)
2. **Solid Colors** (Red, Green, Blue, White, Black - 1 second each)
3. **Color Bars** (2 seconds)
4. **Red-to-Green Gradient** (2 seconds)
Finally, clears the screen to black.

### `lcd-test info`

Dumps `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` from `/dev/fb0`
(resolution, bpp, pixel field layout, physical mm size, memory
layout) — and, if `/dev/ark_display` exists (see
`Limcet Hardware/ark_display.c`), its `ARKDISP_GET_SCREEN_INFO` reply
too, so the two can be cross-checked against each other. Useful for
confirming the panel's real geometry independent of anything
userspace/Qt reports.

```
/ # lcd-test info
```

### `lcd-test fill <red|green|blue|white|black|r,g,b>`

Fills the entire visible framebuffer with a solid color — the simplest
possible "is the panel actually displaying anything" test, bypassing
all userspace UI entirely.

```
/ # lcd-test fill red
/ # lcd-test fill 128,64,200
```

### `lcd-test bars`

Draws classic vertical color bars (white/yellow/cyan/green/magenta/
red/blue/black) across the full width — good for spotting stuck
channels, wrong bit-depth/field-order, or panel timing issues that a
single solid fill won't reveal.

### `lcd-test gradient`

Draws a horizontal red→green gradient — useful for spotting banding
or bit-depth truncation.

## Notes

- Pixel packing reads the actual `red`/`green`/`blue` `fb_bitfield`
  offsets/lengths from `FBIOGET_VSCREENINFO` at runtime — no hardcoded
  RGB565/RGB888 assumption, so it adapts automatically to whatever
  format the `ark1668_lcdfb` driver reports.
- Supports 16/24/32 bits-per-pixel; anything else is rejected with a
  clear error rather than silently corrupting the framebuffer.
- Nothing here writes through `/dev/ark_display` — `info` only reads
  from it, to sanity-check the geometry fix without depending on it.
- **Fixed 2026-07-14 — panning/double-buffering bug.** `fill`/`bars`/
  `gradient` previously always wrote starting at mmap offset 0, ignoring
  `FBIOGET_VSCREENINFO`'s `xoffset`/`yoffset`. On a double/triple-buffered
  fb (`yres_virtual > yres`) with something else (e.g. a QWS/Qt
  compositor) currently panned to a nonzero offset, that write landed on
  an off-screen back buffer — the tool reported success, but nothing
  appeared on the panel. This is the likely explanation if `lcd-test`
  showed nothing even though a raw `dd`-style write to `/dev/fb0` was
  visible: a large enough raw write can spill past one buffer's worth of
  memory into the next (currently visible) one by accident, while a
  single-buffer-sized `fill` never would. `open_fb()` now force-pans to
  `(0,0)` via `FBIOPAN_DISPLAY` before writing (falling back to writing
  at the reported offset, with a warning, if panning isn't supported),
  and `info` now prints `xoffset`/`yoffset` directly so this is visible
  at a glance next time.
