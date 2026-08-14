# Framebuffer Alpha/Channel-Order Test (`fb-alpha-test`)

A static ARM tool that paints 6 labeled horizontal bands directly into
`/dev/fb0`, to empirically determine whether the LCDC's OSD1 layer
actually implements alpha blending and channel order the way the kernel
claims via `FBIOGET_VSCREENINFO`.

## Why this exists

`MsnCoreApp` shows a red tint on UI elements (icons, anti-aliased/
alpha-blended widgets) but **not** on the flat opaque background — see
`docs/1.7_DISPLAY_SUBSYSTEM.md`. A live register test (clearing bit 17 of
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

**Note (2026-07-19, later same session):** an earlier revision of this
README and `lcd-blend-sweep.sh` claimed `rgb_order` was only a 2-bit
field at `OSD1_CTL[22:21]` and had never been tested — that was wrong,
based on an unverified Ghidra parameter-order guess (assumed stock's
`ark_disp_set_osd_format()` argument order exactly matched our own
driver's parameter names without checking it). A kernel debug-proc help
string found in `vmlinux.elf`'s strings (`"rgb_order: 0=rgb, 1=rbg,
2=grb, 3=gbr, 4=brg, 5=bgr"`) proves `rgb_order` needs 6 values (3
bits), contradicting that claim. The original code (`rgb_order` = 3-bit
field at bits `[20:18]`) was correct all along; the erroneous "fix" was
reverted in `linux-arkmicro` `926336ce7`. This means the `rgb_order`
sweep documented above (0–7, including all 6 real values) really was
testing the correct field, and is genuinely exhausted, not still open.

## Ground truth from real stock hardware (2026-07-19) — investigation resolved

Got a root shell on real stock firmware via the `msn_autocopy` telnetd
payload (`payloads/msn_autocopy/README.md`, no separate SD card build
needed — the user telnetted directly into the already-installed
payload). With stock's UI showing correctly-blended elements on
screen, read the three relevant registers with stock's own `busybox
devmem`:

```
devmem 0xe0500060 32   ->  0x03000204   (MODE_LCD_REG0)
devmem 0xe0500064 32   ->  0x00033001   (MODE_LCD_REG1)
devmem 0xe0500074 32   ->  0x000260ff   (OSD1_CTL)
```

**This settles the investigation, and in an unexpected direction:**
`OSD1_CTL` is byte-for-byte identical to what our own board already
reads, `MODE_LCD_REG1`'s OSD1 bits match too, and `MODE_LCD_REG0`'s
`blend_mode` is **`0`** on stock — not `1`, not `9`/`10`/`14`. This is
the exact value our driver's original flat literal already produced
*before* any of this session's LCD register fixes. **The LCDC
hardware register configuration was never the bug** — every candidate
swept in this document (`blend_mode`, `rgb_order`, `format`,
`Y2R_COEF`, `ALPHA1_0_VIDEO_OSD1`) could never have fixed this, since
there was never a wrong register value to find. The `blend_mode=1`
kernel change has been reverted (`linux-arkmicro` `41eaa6463`).

The real bug is almost certainly at the pixel-data level: stock uses
`QWS_DISPLAY=directfb` (software-composites to fully opaque pixels
before ever touching the framebuffer, likely never exercising real
hardware alpha blending at all), while this build uses
`QWS_DISPLAY=linuxfb` (switched away from `directfb` to avoid a
GPU/`galcore` crash class). Qt's LinuxFB path may write genuinely
semi-transparent pixel data expecting hardware blending to finish the
job — hardware that this investigation showed doesn't reliably work
correctly on this silicon regardless of register configuration. See
`docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md` §1b for the full writeup
and redirected next steps (now a Qt/userspace investigation, not a
kernel-driver one).
