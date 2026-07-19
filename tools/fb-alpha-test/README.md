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

## `lcd-blend-sweep.sh` — live register sweep (2026-07-19 findings so far)

Once `fb-alpha-test` proved the bug is real (not photo-guesswork), the
investigation moved to live `devmem` register pokes on `LCDC`
(`0xe0500000`) to find the actual broken/missing register value, without
needing a kernel rebuild per attempt. Findings so far:

- **Opaque pixels (alpha=255) always render correctly**, on every
  combination tried. Only *partial*-alpha pixels are wrong — this is
  true both before and after every fix below.
- **`blend_mode`** (`MODE_LCD_REG0` bits `[15:12]`, offset `0x60`) was
  hardcoded to `0` by our driver (fixed in `linux-arkmicro` `063c5be8c`
  to `1`, matching stock's struct-init default). Empirically swept
  0–15 on hardware: **modes 9, 10, and 14 all turn on real per-pixel
  alpha blending** (`fb-alpha-test` shows 6 distinct bands instead of 4
  merged ones) — but the *color* is still wrong for every partial-alpha
  band once blending is active. `1` (the "safe" stock default) does
  *not* by itself enable real blending on our reconstruction — it's
  unclear why 9/10/14 specifically work; no register documentation
  found to explain the encoding.
- **`rgb_order`** (`OSD1_CTL` bits `[20:18]`, offset `0x74`) swept 0–7
  with `blend_mode=9`: band 2 (half-alpha red) came out dark-green
  (0,1), light-green (2,3), brown (4,5), dark-green (6,7) — **never** a
  correct dim red. `rgb_order` alone does not fix it.
- **`format`** (`OSD1_CTL` bits `[15:12]`) — not yet tested empirically
  at time of writing; `lcd-blend-sweep.sh` phase 1 covers this.
- **`Y2R_COEF321`/`654`/`7`** (offsets `0x11c`/`0x120`/`0x124`) —
  **ruled out (2026-07-19, later Ghidra pass).** Found stock's real
  `ark_disp_set_lcd_panel_type()` (`vmlinux.elf @ 0x802e0a78`)
  hardcoding these exact same literals: `COEF321=0x1a916d2a`,
  `COEF654=0x1d12e060`, `COEF7` ORs in `0x1029` (`(1<<12)|41`) after
  clearing bits `0x33c0`/`0x3f` first. Our driver's values
  (`(425<<20)|(91<<10)|(298<<0)` etc.) compute to the **exact same
  literals** — confirmed via direct calculation, byte-for-byte match.
  The one difference is stock explicitly clears those bits before
  OR'ing in `COEF7`, ours only ORs — theoretically could matter if
  those bits are non-zero when `set_par()` runs, but given the other
  two registers match exactly, this is now a low-priority loose end,
  not the main suspect. `lcd-blend-sweep.sh` phase 2 still zeroes them
  as a quick sanity check, but don't expect it to be the fix.
- **`ALPHA1_0_VIDEO_OSD1`** (offset `0x24`) — **very likely unused for
  this board** (2026-07-19, later Ghidra pass). Systematically traced
  every access to the LCDC MMIO base (`0xf6800000`-pattern) across the
  entire ~100KB display-driver code range (`ark_disp_*`/
  `ark1668_lcdc_*`/`ark_fb_*`/`ark168vin_*`, `vmlinux.elf @
  0x802d83d8`-`0x802ec380`) and found zero writes to this offset.
  `ark_disp_set_layer_cfg()` (the "apply full layer config to
  hardware" function, `@ 0x802db8d4`) calls a comprehensive list of
  per-layer setters (alpha, alpha_blend_en, per_pix_alpha_blend_en,
  blend_mode, colorkey, priority, layer_cut) and none of them target
  this offset. `lcd-blend-sweep.sh` phase 3 still probes it for
  completeness, but don't expect it to be the fix either.
- Checked whether stock's actual `MsnCoreApp`/`libarkadapt.so`/
  `libarkcmn.so` binaries (all in `firmware_dumps/Prado firmware dump/
  mtd6_rootfs/`) hardcode a correct `format`/`rgb_order` value when
  calling the vendor `VIN_SET_WINDOW_FORMAT` ioctl (`_IO('O', 59)` =
  `0x4f3b`) — `libarkadapt.so`/`libarkcmn.so` are stripped with no
  ioctl-related symbols found by name; `MsnCoreApp` itself is
  unstripped but had no vendor-ioctl symbols either, meaning the vendor
  ioctl call (if any) is buried in one of the stripped libraries.
  Not pursued further — would need real binary-level reverse
  engineering (searching for the `0x4f3b` immediate near `ioctl()`
  calls), left as a future option if the register sweep doesn't pan
  out.

Run `sh lcd-blend-sweep.sh` on-device (kills `MsnCoreApp` first) to
continue this sweep — it pauses after each register value change so you
can run `fb-alpha-test` and report what bands 1 and 2 look like.
