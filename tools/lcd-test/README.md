# lcd-test

Live LCD/framebuffer diagnostic tool — same purpose/style as
`tools/i2c-scan/` and `tools/ark1680-ts-test/`: a static ARM binary to
run at the live `/ #` root shell. Tests the raw kernel framebuffer
(`/dev/fb0`) directly, with no Qt/QWS server involved — so it works even
while `MsnCoreApp`/`LCDTest -qws` are segfaulting (see
`docs/1.8_ARK1680_TS_REVERSE_ENGINEERING.md` → "`MsnCoreApp` segfault").

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
the panning bug noted below) if the panel doesn't visibly change.

No arguments, no subcommands — just run it:

```
/ # lcd-test
```

It dumps `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` from `/dev/fb0`
(resolution, bpp, pixel field layout, physical mm size, memory layout)
and, if `/dev/ark_display` exists (see `Limcet Hardware/ark_display.c`),
its `ARKDISP_GET_SCREEN_INFO` reply too, so the two can be cross-checked
against each other — then cycles through the whole test sequence on its
own, pausing 2 seconds between each step and printing what it just drew
so the console log and the panel can be watched side by side:

1. Solid fills: red, green, blue, white, black — the simplest possible
   "is the panel actually displaying anything" test.
2. Vertical color bars (white/yellow/cyan/green/magenta/red/blue/black)
   — good for spotting stuck channels, wrong bit-depth/field-order, or
   panel timing issues a single solid fill won't reveal.
3. A horizontal red→green gradient — useful for spotting banding or
   bit-depth truncation.

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
- **Updated 2026-07-16 — match stock `LCDTest`'s init sequence.** Even
  after the panning fix above, this tool's writes were still never
  confirmed visible on real hardware — while the factory `LCDTest -qws`
  binary (Qt + DirectFB) *did* render correctly, see
  `docs/1.7_DISPLAY_SUBSYSTEM.md`'s 2026-07-16 milestone entry. Traced the
  difference: DirectFB's `fbdev` system module applies its mode via
  `FBIOPUT_VSCREENINFO`, which is what actually invokes the kernel
  driver's `.fb_set_par` hook (`ark1668_lcdfb_set_par()` in
  `ark1668_lcdfb.c`) — the function that programs *and enables* the
  OSD1 display layer. This tool previously only ever called
  `FBIOGET_VSCREENINFO` and (for the panning fix) `FBIOPAN_DISPLAY`
  (`.fb_pan_display`, a different hook that only repoints the OSD1
  address, doesn't touch the enable bit) — never `FBIOPUT_VSCREENINFO`.
  `open_fb()` now re-issues `FBIOPUT_VSCREENINFO` with the info it just
  read back (not changing the mode, just re-applying it) before writing,
  matching the one userspace path already confirmed to work. Not yet
  independently re-confirmed against real hardware with this exact
  change — see the open item in `docs/1.7_DISPLAY_SUBSYSTEM.md`.
