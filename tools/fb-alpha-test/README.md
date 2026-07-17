# Framebuffer Alpha/Channel-Order Test (`fb-alpha-test`)

A static ARM tool that paints 6 labeled horizontal bands directly into
`/dev/fb0`, to empirically determine whether the LCDC's OSD1 layer
actually implements alpha blending and channel order the way the kernel
claims via `FBIOGET_VSCREENINFO`.

## Why this exists

`MsnCoreApp` shows a red tint on UI elements (icons, anti-aliased/
alpha-blended widgets) but **not** on the flat opaque background — see
`docs/DISPLAY_SUBSYSTEM.md`. A live register test (clearing bit 17 of
`ARK1668_LCDC_OSD1_CTL`) turned the *entire* screen green instead of only
affecting blended elements — ruling out a simple hardware-register fix,
since any register change is necessarily global to the whole layer, but
the actual symptom is selective. That points at a mismatch between what
Qt assumes about the pixel format and what the hardware actually does
with the alpha byte, which needs direct empirical testing, not more
register-guessing.

## Usage

```bash
killall MsnCoreApp   # stop anything else that might repaint over this
chmod +x ./fb-alpha-test
./fb-alpha-test
```

Prints the kernel's reported channel layout, then draws 6 bands
top-to-bottom and prints what each one *should* look like:

0. Opaque mid-gray (sanity reference)
1. Opaque solid red (sanity check for basic RGB order)
2. **Half-alpha red — the actual bug case**, same kind of pixel
   `MsnCoreApp`'s blended UI elements use
3. Half-alpha green (control, compare against band 2)
4. Raw literal bytes `[ff,00,00,80]`, ignoring whatever the kernel
   claims the channel offsets are
5. Raw literal bytes `[80,00,00,ff]` (reverse byte order of band 4)

## Reading the result

Compare each band's *actual* on-screen color against its printed label:

- If band 0 isn't neutral gray or band 1 isn't pure red, the basic RGB
  channel order itself is wrong (not just alpha).
- If band 2 (half-alpha red) looks visibly different/tinted compared to
  band 3 (half-alpha green) in a way that isn't just "dimmer" — e.g. red
  looks wrong while green looks like a sensible faded green — that's a
  strong sign the alpha byte's *position* is being misinterpreted
  specifically when it overlaps with a live red byte, which is exactly
  what would produce "red-tinted elements" in the real app.
- Bands 4 and 5 show the true hardware behavior for a specific byte
  arrangement, independent of anything the kernel declares — whichever
  one produces a "sensible half-red" look (rather than a wrong color
  entirely) tells us the real byte order to fix the kernel's reported
  `fb_var_screeninfo` (or Qt's assumptions) to match.

This tool only writes to the framebuffer — nothing persists across a
reboot or restarting `MsnCoreApp`, so it's safe to run repeatedly.
