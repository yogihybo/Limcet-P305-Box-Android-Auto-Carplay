# U-Boot Boot Logo & Reverse-Engineered Feature Ports

Summary of work to (1) get a boot logo showing on the compiled u-boot, and
(2) port a handful of vendor-only features out of the stock production
binary (`mtd1_uboot.bin`), which has no released source anywhere. Covers
the reverse-engineering toolchain, what was found, what got ported, what
was deliberately left out, and how to test it.

Build tree referenced throughout: `/home/osboxes/Downloads/linux-arkmicro/u-boot`
(full buildable u-boot 2018.07-based source with the `ark1668_limcet_p305`
board). The `linux-arkmicro Reference/u-boot` folder in this repo is only a
partial vendor overlay (board/arch/driver files) — it was never the full
picture; `cmd/`, `common/`, `lib/` etc. live only in the build tree above.

---

## 1. The original question: why no boot logo?

The compiled `ark1668_limcet_p305` u-boot showed a blank/white screen where
stock firmware shows a logo at the `=>` prompt. Investigation ruled out
several theories before landing on the real cause:

- **Not a Stepldr leftover.** Initial theory was that the pre-u-boot stage
  (`Stepldr.bin`/`Nboot.bin`, both proprietary/NAND-resident) draws the logo
  and u-boot just doesn't disturb it. Disproven by decompiling the stock
  binary's command table.
- **Not decoded by u-boot's own `disconfig`/`display_updatelogo()`.** That
  code (present in source) only draws a small hard-coded "updating..."
  progress bar during a firmware-update flow, and the live device's saved
  env (`bootcmd=run nandboot`) doesn't even reach it on a normal boot.
- **Real cause, confirmed via disassembly:** stock u-boot's command table
  (recovered from `mtd1_uboot.bin` — see §3) has `jpeghw`/`jpeg decode`
  commands that drive the SoC's dedicated hardware JPEG decoder
  (`JPEG_BASE = 0xE0200000`) to decode the `bootlogo` NAND partition
  (a real JPEG, 800×480) directly into the OSD framebuffer. None of that
  exists in the source tree at all. The compiled build's screen was never
  being fed valid pixel data — hence white, not black (classic TFT
  no-signal state, not framebuffer corruption).

## 2. The shipped fix: SD-card raw framebuffer bootlogo

Since there's no JPEG decoder in u-boot source (and porting the stock
hardware-JPEG-decoder driver was assessed as high-risk — see §4.2), the
logo is now shown via a much simpler path that reuses code already in the
board file:

1. **Offline, once**: `convert_bootlogo.py` (repo root) converts a JPEG
   (e.g. the dumped `Prado firmware reconstructed/mtd8_bootlogo/bootlogo`,
   or any 800×480 image) into a raw 32bpp pixel buffer.
   - Pixel format: each pixel packed as `(0xFF<<24)|(R<<16)|(G<<8)|B`,
     written little-endian — this is the same convention the existing
     `display_updatelogo()` progress-bar code already used for its
     `0xffffffff`/`0xff00ff00` color constants, so it's a proven match for
     this panel's `DISP_RGB_888` + `RGB_MODE_BGR` config.
   - Output is exactly `width * height * 4` bytes (1,536,000 for 800×480).
2. **On the SD card**: the converted file goes on the FAT boot partition
   as `bootlogo.raw`, next to `UBOOT.BIN`.
3. **In u-boot** (`board/arkmicro/ark1668_limcet_p305/ark1668_display_cfg.c`):
   - `ark_show_bootlogo()` — calls `ark_display_init(SCREEN_QUN700)` for
     panel/clock/port bring-up, then `display_bootlogo_from_sd()`.
   - `display_bootlogo_from_sd()` — `fatload mmc 0:1 <addr> bootlogo.raw`,
     then points `OSD1_LAYER` (the main content layer, previously left
     disabled) at it full-screen via the existing
     `ark_set_osd_image()`/`ark_set_osd_addr()`/`ark_osd_en_layer()`
     primitives (same calls already used for the small update-progress
     overlay on `OSD2_LAYER`).
   - Wired into `board_late_init()` in `ark1668.c` as the first call, so
     it runs before the console banner / autoboot countdown.

This is why the panel now gets valid pixel data regardless of whether the
NAND `bootlogo` partition or any JPEG hardware is involved at all.

## 3. Reverse-engineering toolchain

No root access was available, so everything was installed portably into
`~/tools/`:

- **radare2** — extracted directly from the official `.deb` via
  `dpkg-deb -x` (no install, just unpacked files + `LD_LIBRARY_PATH`).
- **Ghidra 12.1.2** — downloaded release zip, unpacked.
- **Temurin JDK 21** — portable tarball (Ghidra's only real dependency).

Ghidra headless workflow used throughout:

```bash
export JAVA_HOME=~/tools/jdk/jdk-21.0.11+10
export PATH="$JAVA_HOME/bin:$PATH"
GHIDRA=~/tools/ghidra/ghidra_12.1.2_PUBLIC

# one-time import + full auto-analysis
"$GHIDRA/support/analyzeHeadless" ~/tools/ghidra_project stock_uboot \
  -import "Prado firmware dump/mtd1-mtd2_uboot/mtd1_uboot.bin" \
  -processor "ARM:LE:32:v7" -loader BinaryLoader -loader-baseAddr 0x30000

# subsequent runs: decompile specific functions to a text file via a
# small custom GhidraScript (DumpFunc2.java-style — ensures a Function
# exists at each address, then calls the decompiler and writes the C)
"$GHIDRA/support/analyzeHeadless" ~/tools/ghidra_project stock_uboot \
  -process mtd1_uboot.bin -noanalysis \
  -scriptPath <dir> -postScript DumpFunc2.java "0xADDR1,0xADDR2,..." out.txt
```

Load address `0x30000` matches the stock binary's documented load address
(see `docs/uboot_build.md`).

### Command table recovery

The stock binary is stripped (no symbol table), so every function starts
out as `FUN_00xxxxxx`. The command table was located by finding the
`bootnand` string's address, then searching the binary for a 32-bit
little-endian word matching that address — which lands inside a
`cmd_tbl_s`-shaped array (`{name, maxargs, repeatable, cmd, usage}`,
5 words / 0x14 bytes per entry, matching the classic pre-Kconfig
u-boot command table layout). Walking that array by hand (with a couple of
early field-order mistakes, since corrected — see conversation) recovered
real names for every custom command:

| Command | Address | What it does |
|---|---|---|
| `disconfig` | `0x68bec` | Display config (fuller than the reference `do_disconfig` in source — calls into `LcdArgInFlash`/`ui_scaler_type` chain, see §4.3) |
| `gpiotest` | `0x69880` | 3-mode GPIO self-test (0=input watch, 1=output blink, 2=reuses JPEG clock init + registers dummy IRQ callbacks) |
| `jpeghw` | `0x69c28` → `0x69b10` | Hardware JPEG decode: writes `dec_rd_base_addr`/dest registers, sets the `START` bit, waits on completion (stock: via interrupt) |
| `pmem` | `0x6c404` | Hex memory dump |
| `regw`/`regr` | `0x6c244`/`0x6c0d8` | Generic register peek/poke across 6 blocks (opcode selects `LCD_BASE`/`SYS_BASE`/`ITU656_BASE`/`VICL_BASE`/`VICH_BASE`/`JPEG_BASE`) |
| `itu656` | `0x6e9f0` | NTSC/PAL composite video-input timing setup |

Full command table also confirmed dozens of standard upstream u-boot
commands (`go`, `bootspi`, `bootz`, `ext4load`, `fatload`, `md`, `mw`,
`ubi`, `usb`, etc.) are unchanged from mainline — those weren't touched.

### Full-binary decompile + source matching

All 750 functions in the stock binary were decompiled in one pass and
saved to `docs/re_stock_uboot/`:
- `full_decompile.c` (1.1MB) — every function, address order
- `function_index.tsv` — address/size/name + up to 4 string hints per
  function (Ghidra auto-labels string data by content, which substitutes
  for the missing symbol names)

These were cross-referenced against the current source tree by string
matching (grep each function's referenced string literals against
`*.c` in the build tree, with prefix-truncation fallback to handle
wording drift between the stock binary's **U-Boot 2012.10** base and the
current **2018.07** tree). Result: 197/257 functions-with-strings matched
cleanly to an existing source file (confirming most of the binary —
UBI, USB, FAT/ext2/ext4, zlib, SHA1, NAND BBT — is unmodified upstream
code). The ~60 real no-match functions are the genuinely vendor-custom
ones; see §4 for what was done with them.

One important side-finding from this pass: `ARK_DISPLAY_ALL_MODE` is
`#define`d `0` in `ark1668_lcd.h` and gates ~8 blocks in `ark1668_lcd.c`
(gamma, video/OSD color scaling, TV-encoder init paths, and the
`display_updatepara` struct fields for `ui_scaler_type`/`itu656bypinfo`/
`special_info`). **Left off** per explicit decision — some of the gated
functions are dead even if enabled (`ark_set_gamma()` only called from a
commented-out line), and turning it on doesn't restore the arkdata.ini
loading or reversing-camera hooks anyway (those have zero source presence,
gated or not).

## 4. What got ported, and the risk call on each

Everything below is additive (new files/functions), doesn't modify
existing display/boot logic, and builds warning-clean. **None of it has
been tested on real hardware.**

### 4.1 Low risk — shipped

**`regr`/`regw`/`pmem`** (`ark1668_display_cfg.c`) — plain address
peek/poke across the 6 register blocks `regw`/`regr` used in stock.
Pure reads/writes, no protocol to get wrong.

**`gpiotest 0`/`1`** (`ark1668_debug_cmds.c`) — input-watch and
output-blink GPIO tests, ported faithfully from the decompiled bit-bang
logic on `GPIO_BASE = 0xE4600000`. Stock's versions loop forever with no
escape; `ctrlc()` checks were added as a deliberate improvement over the
original.

**`bootlogofind`** (`ark1668_display_cfg.c`) — ported from stock's
`FUN_0006bf68`: tries the NAND `bootlogo` partition first, checks for a
JPEG SOI marker (`0xFF 0xD8`); if missing, falls back to
`fatload mmc <0/1/2>:1 bootlogo` on SD. Kept as a diagnostic only (no
JPEG decoder to actually display what it finds), but useful for
confirming whether valid bootlogo JPEG bytes are present anywhere.

### 4.2 Moderate risk — shipped with a deliberate deviation from stock

**`jpeghw <src_hex> <dst_hex>`** (`ark1668_debug_cmds.c`) — real hardware
JPEG decode. Register sequence and offsets are decompiled faithfully:

```
JPEG_BASE (0xE0200000) + 0x3c  INTCLR
                        + 0x2c  CTRL
                        + 0x04  mode/table select
                        + 0x50  COUNT
                        + 0x38  INTMASK
                        + 0x5c  dec_rd_base_addr  <- source JPEG bytes
                        + 0x24  dest Y-plane
                        + 0x28  dest chroma-plane (dest + 0x200000)
                        + 0x30  START (bit 31)
                        + 0x34  status (bit0=done bit2=error)
```

**Deviation**: stock is interrupt-driven — it registers an ISR against
the SoC's VIC (line 10, `JPEG_INT`) via `FUN_0006a11c`/`FUN_00069ff8`,
and the actual decoded-width/height values come from that ISR
(`FUN_00069980`, fully decompiled — confirms status bit 0 = success,
bit 2 = error, decoded W/H read from the upper 16 bits of
`JPEG_BASE+0x04`/`+0x0c`). Porting the ARM IRQ exception-vector plumbing
that would require was judged too risky to blind-port (wrong offset
there hangs/crashes the CPU, no safe incremental test). The shipped
version **polls** `JPEG_BASE+0x34` directly instead, using the exact same
done/error bit logic the ISR uses — same hardware protocol, different
(safer) wait mechanism.

**`itu656`** (`ark1668_debug_cmds.c`) — NTSC composite video-input timing.
The constants are real, confirmed from two independent sources
(`display/arkdata.ini` and the stock binary's own built-in default table
— they match exactly): `ModeControl=0x1D80`, `VBP`/`VFP`/etc. The register
bit-packing (which field goes at which shift, into which of
`LCD_BASE+0x3d0..0x3e4`) was transcribed field-for-field from the
decompiled `FUN_0006e870`, cross-checked using the fact that the PAL
register block mirrors the NTSC one exactly (self-validating). **One
block intentionally left out**: a section in stock's `FUN_0006e9f0`
between the timing setup and the final enable, gated on unresolved
pointers (`DAT_0006ea90/94/98`) that look like current screen/resolution
state — flagged in a code comment rather than guessed.

### 4.3 High risk — explicitly scoped out

**`gpiotest 2`** and the VIC/ARM-IRQ-vector infrastructure generally —
stubbed with a message instead of ported. Same risk as above.

**The reversing-camera / video-scaler / DMA pipeline** behind stock's
`FUN_000684d0` (called from the real `disconfig`) — decompiled far enough
to see the shape of it (an 8-way branch, `FUN_0006e7b0`, each arm doing
20-40 bit-packed writes into unlabeled scaler/DMA-controller registers,
feeding DMA engines that write decoded video frames to **hardcoded raw
physical addresses** — `0xE000000`, `0xB400000`, `0xBE00000`). Explicitly
excluded per user decision: not used on this device, and the failure mode
of getting it wrong isn't "camera doesn't work" — it's a misconfigured DMA
engine writing outside its buffer, a real corruption/hang risk with no
datasheet to check against.

**The full stock ini-parser object** (`FUN_0006f97c`/`f910`/`f6e0` — a
genuine generic INI library with UTF BOM detection and hash-table key
lookup) — not replicated byte-for-byte. See §5: a much simpler
from-scratch reader was written instead, matching *observable* behavior
(iterate lines, look up by key, parse as int) rather than the internal
hash-table bucket layout, since callers can't tell the difference and
matching internals would have been large effort for zero behavioral
gain. Also, with the camera pipeline and `ARK_DISPLAY_ALL_MODE` both out
of scope, most of what the full stock parser feeds (gamma, scaler type,
itu656 calibration, video-processing brightness/contrast) has nothing
left to consume it anyway.

### 4.4 Empirical confirmation: LCD path doesn't use the VIC/IRQ at all

The VIC/IRQ scoping decision in §4.3 was based on source/decompile
reading alone at the time. It's since been confirmed directly against
real hardware register reads, using the already-shipped `regr` command,
after the boot logo was already confirmed working on-screen.

Source basis for the claim: `ark_disp_wait_lcd_frame_int()` (already in
`ark1668_lcd.c`, called from `display_updatelogo()` as part of
`ark_display_init()`) is a plain busy-poll, not an interrupt handler,
despite the name:

```c
void ark_disp_wait_lcd_frame_int(void)
{
	// wait until LCD timing point intr happens (which is VSync here)
	rLCD_INTERRUPT_STATUS = 0;
	while(!(rLCD_INTERRUPT_STATUS & 0x01));
	// the timing point is set at bit22-21 on CLCD_CONTROL reg
}
```

`rLCD_CONTROL` is `LCD_BASE+0x004`, `rLCD_INTERRUPT_STATUS` is
`LCD_BASE+0x180` — both plain memory-mapped registers on the LCD
controller itself, no VIC/ARM-IRQ-vector involvement in the source.

**Hardware trace, taken at the `=>` prompt after the logo was already on
screen:**

```
=> regr 0 0x4
[op=0] reg 0x04 = 0x03600081
=> regr 0 0x180
[op=0] reg 0x180 = 0x00000033
=> regr 0 0x180
[op=0] reg 0x180 = 0x00000033
=> regr 3 0x14
[op=3] reg 0x14 = 0x00000000
=> regr 4 0x14
[op=4] reg 0x14 = 0x00000000
```

Reading:
- **`rLCD_CONTROL = 0x03600081`** — bits 21-22 read as `0b11`, matching
  the source comment that this field selects VSync as the monitored
  timing point (consistent with `ark_disp_wait_lcd_frame_int()` actually
  being the function in effect, not the TVENC/bit12-11 variant — correct
  for an RGB LCD panel, not a CVBS/TV-encoder output). Bit 0 set (LCD
  enable), plus a couple of other control flags (bits 7, 24-25) not
  documented in what source we have, but nothing that correlates with any
  visible problem.
- **`rLCD_INTERRUPT_STATUS = 0x33`, identical on two consecutive reads**
  — expected, not a fault. `0x33 = 0b00110011`; bit 0 (`0x01`, the exact
  bit the wait function polls) is **set**, meaning the LCD controller is
  actively latching VSync events right now — direct, independent
  confirmation (from a status register, not just "the picture looks
  right") that the panel is actively scanning. It reads identically
  because nothing is clearing/re-arming it between reads outside the
  wait function's own poll loop — a level-latched flag staying latched
  when nothing clears it is correct, not a hang. Bits 1/4/5 are other
  status flags with no documented meaning in the source available, but
  uncorrelated with any visible fault.
- **`VICL`/`VICH` enable-mask (`op=3`/`op=4` @ `+0x14`) both
  `0x00000000`** — the key result. Zero interrupt lines enabled on
  either VIC, read directly from silicon, while the display is
  demonstrably working. This confirms empirically what the source read
  already implied: **the LCD/boot-logo path never touches the VIC or
  ARM IRQ-vector infrastructure.** The interrupt-vector work scoped out
  in §4.3 was correctly assessed as unnecessary for anything currently
  working on this device — it's specific to the (unported) `jpeghw`
  interrupt-driven path and `gpiotest 2`, not to display.

## 5. LCD timing fix + `arkdata.ini` runtime reader

### 5.1 Compiled defaults didn't match the real calibration

Comparing the hardcoded `SCREEN_QUN700` entry in `screens[]`
(`ark1668_display_cfg.c`) against `display/arkdata.ini`'s
`[LCD_TIMMING]`/`[LCD_CLOCK]` sections found real, non-trivial mismatches:

| Field | Compiled default | `arkdata.ini` |
|---|---|---|
| VBP | 40 | 29 |
| VFP | 36 | 25 |
| HFP | 32 | 25 |
| HSW | 41 | 54 |
| Pixel clock | 0 (derived) | 330,000,000 Hz |
| CLKDIV1 | 13 | 11 |

(`Width`/`Height`/`VSW`/`HBP` and sync polarities already matched.)

Differences this size (30%+ on several porch/sync-width fields) risk a
shifted/mistimed picture or a complete sync failure, independent of
anything OSD/framebuffer-related. **Fixed**: the `SCREEN_QUN700` struct
literal now uses the `arkdata.ini` values; the original values are kept
as a comment directly above for easy revert if the new timing doesn't
sync on real hardware. `tvout_format`/`tvenc` were deliberately left
alone — no clean 1:1 field mapping to `arkdata.ini`'s `TvoutType`, and
guessing there risked introducing a new bug rather than fixing one.

### 5.2 `arkdata.ini` runtime reader (new)

New file `ark1668_arkdata_ini.c` lets the SD card's `arkdata.ini`
override the compiled LCD timing at boot, instead of requiring a
recompile every time calibration changes:

- `arkdata_ini_load()` — lazily `fatload`s `arkdata.ini` from `mmc 0:1`
  into RAM once.
- `arkdata_ini_get_int(key, base, &out)` — flat line scanner: finds
  `key=value` (leading whitespace tolerated, `;` comments and
  `[Section]` headers just don't match any key so they're implicitly
  skipped, blank values treated as absent).
- `arkdata_apply_lcd_timing(screen)` — overrides
  `vbp`/`vfp`/`vsw`/`hbp`/`hfp`/`hsw`/sync-polarity/clock fields on an
  already-populated `screen_info`. **Fails safe**: missing file or
  missing key just leaves the field at whatever the compiled default
  (now the corrected one, §5.1) already was.
- `arkdatatest <key>` — new command for ad-hoc key lookups.

Wired into `ark_display_init()`, called right before the screen struct is
used, so any `arkdata.ini` present on the SD card's FAT partition
(next to `UBOOT.BIN`) is picked up automatically on the next boot — no
code changes needed to try a different unit's calibration.

**Deliberately not ported**: the stock hash-table/BOM-detection ini
engine (see §4.3) and the gamma/scaler/itu656/carback-camera fields —
consistent with the camera pipeline and `ARK_DISPLAY_ALL_MODE` scope
decisions. Extending `arkdata_apply_lcd_timing`-style overrides to more
fields later is a small, low-risk addition on top of what's here — the
parsing plumbing is already built and doesn't need to change.

### 5.3 Debug logging

Both the bootlogo path and the arkdata.ini path now log their full
decision trail on the serial console (not just success/failure):

- `ark_show_bootlogo()`/`display_bootlogo_from_sd()` — logs the exact
  `fatload` command run, its return code, the reported file size vs. the
  expected `800×480×4` (with a warning if they don't match — catches a
  stale/wrong conversion immediately), and confirms when OSD1 is enabled.
- `arkdata_ini_load()`/`arkdata_apply_lcd_timing()` — logs the `fatload`
  command and result, the reported file size, and **every field**
  old-value → new-value (or "not found, keeping compiled default").
- `#define DEBUG` is set at the top of `ark1668_arkdata_ini.c` only
  (file-scoped, not tree-wide) to also surface the finer-grained
  `debug()`-level per-key parse traces without flooding the console with
  unrelated subsystem noise from a global debug build.

Example boot log:

```
bootlogo: ark_show_bootlogo() starting, screen_id=0
bootlogo: ark_display_init() done
arkdata.ini: applying LCD timing overrides for screen_id=0
arkdata.ini: loading -> `fatload mmc 0:1 0xfe00000 arkdata.ini`
arkdata.ini: fatload reported filesize=0x2a4 (676 bytes)
arkdata.ini: loaded 676 bytes from SD into RAM @ 0xfe00000
arkdata.ini:   VBP      29 (unchanged)
arkdata.ini:   VFP      25 (unchanged)
...
arkdata.ini: done — 12/12 fields overridden from SD card, final timing (...)
bootlogo: loading -> `fatload mmc 0:1 0xfc00000 bootlogo.raw`
bootlogo: fatload reported filesize=0x177000 (1536000 bytes), expected 0x177000 (800x480x32bpp)
bootlogo: pushing OSD1 image 800x480 @ 0xfc00000 (DISP_RGB_888)
bootlogo: OSD1 layer enabled, splash should be visible now
```

## 6. File manifest

**Repo root** (`prado-firmware-reconstruction/`):
- `convert_bootlogo.py` — JPEG → raw 32bpp framebuffer converter
- `make_test_bootlogo.py` — generates an 800×480 "U-boot loading" test
  image (PNG) for exercising the pipeline without a real logo asset
- `test_bootlogo.raw` — the converted output of the above, ready to copy
  onto an SD card as `bootlogo.raw`
- `inject_ark_header.py` — post-build ARK header injection (pre-existing)
- `docs/re_stock_uboot/full_decompile.c` / `function_index.tsv` — full
  stock-binary decompile + searchable index

**Build tree** (`~/Downloads/linux-arkmicro/u-boot/board/arkmicro/ark1668_limcet_p305/`):
- `ark1668.c` — `board_late_init()` now calls `ark_show_bootlogo()` first
- `ark1668_display_cfg.c` — `ark_show_bootlogo()`, `display_bootlogo_from_sd()`,
  `bootlogofind`, `regr`/`regw`/`pmem`, corrected `SCREEN_QUN700` timing
  (old values commented above), `arkdata_apply_lcd_timing()` call added
  to `ark_display_init()`
- `ark1668_debug_cmds.c` (new) — `gpiotest`, `jpeghw`, `itu656`
- `ark1668_arkdata_ini.c` (new) — `arkdata.ini` runtime reader,
  `arkdatatest` command
- `ark1668_lcd.h` — declarations for the above
- `Makefile` — registers the two new `.c` files

## 7. Testing status

**Verified on real hardware** (2026-07-12): the full boot logo pipeline —
SD card FAT read, `convert_bootlogo.py`'s pixel packing, corrected
`SCREEN_QUN700` timing, `ark_display_init()` panel bring-up, and the
`OSD1_LAYER` push — confirmed working end to end. `test_bootlogo.raw`
(the "U-boot loading" test image, §6) displayed correctly, and critically,
the test image's border was **pixel-perfect against the physical screen
edges** — strong confirmation that the corrected VBP/VFP/HFP/HSW/clock
values pulled from `arkdata.ini` (§5.1) are genuinely accurate, not just
"close enough to sync." A wrong porch/sync value would show up exactly as
border cropping or offset, so this is about as strong a validation as
that fix could get from visual inspection alone.

Not yet separately confirmed: whether `arkdata.ini` itself was present on
the SD card during this test (i.e. whether the runtime override path in
§5.2 fired, vs. the corrected compiled defaults alone being sufficient) —
worth checking the serial log's `arkdata.ini:` lines specifically if that
distinction matters.

**Verified (build only, not yet exercised on hardware)**: everything
builds warning-clean (`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-`),
`inject_ark_header.py` produces a valid `UBOOT.BIN` each time.

**Not yet tested on real hardware**, roughly safest-first:
1. `regr`/`gpiotest 0`/`1`/`bootlogofind`/`arkdatatest` — pure reads, safe
   to try
2. `jpeghw`/`itu656` — real hardware register writes with inferred (not
   datasheet-confirmed) semantics

To reproduce the boot logo test: copy `UBOOT.BIN`, `test_bootlogo.raw`
(renamed to `bootlogo.raw` on the card), and optionally `arkdata.ini`
onto the SD card's FAT partition next to each other, boot with the
serial console attached, and watch for the log trail in §5.3.

## 8. USB boot ideas (proposed, not yet implemented)

Motivated purely by iteration speed — every test cycle currently means
physically removing and reflashing the SD card. With USB mass storage now
confirmed working in u-boot (§ dual-port USB fix; real hardware test read
a SanDisk flash drive's vendor/capacity/partition table correctly), two
ideas were discussed for using a USB stick instead, at different layers.

### 8.1 What's a hard constraint vs. what's not

**U-Boot itself cannot be loaded from USB.** The boot chain's first two
stages — the SoC's boot ROM, then the proprietary NAND-resident
`Nboot.bin`/`Stepldr.bin` — are fixed and load `UBOOT.BIN` from the SD
card specifically (see `docs/uboot_build.md`'s boot chain). There's no
evidence anywhere in this project that the boot ROM supports USB as a
boot source, so the SD card can't be removed from the loop entirely —
u-boot itself always has to come from there.

Everything u-boot itself loads *after* it's running is a different
story — that's just `fatload`, and it already works identically for any
block-device interface u-boot supports (`mmc`, `usb`, ...).

### 8.2 Idea A — load everything except U-Boot from USB

Keep the SD card minimal (just `UBOOT.BIN`, rarely touched), and point
the boot flow at USB for everything that actually changes during
iteration: `zImage`, the DTB, `uEnv.txt`, `bootlogo.raw`, `arkdata.ini`.
Mechanically this is a small change — `fatload usb 0:1 <addr> <file>`
is the same FAT/`CONFIG_CMD_FAT` code path as the existing `fatload mmc
0:1 ...` calls throughout `CONFIG_BOOTCOMMAND` and the various `.c`
files in this project (`ark1668_display_cfg.c`, `ark1668_arkdata_ini.c`),
just a different interface string. The natural shape: try USB first,
fall back to the existing SD-based path if no USB stick is present, so
nothing breaks when testing without one plugged in.

Not yet implemented. Lower risk than 8.2 below — it's the same `fatload`
mechanism already used and tested throughout this project, just against
a different (already-proven-working) block device.

### 8.3 Idea B — chainload a second U-Boot from USB

More ambitious: iterate on U-Boot *itself* without touching the SD card
at all, by having the SD-resident U-Boot load a freshly-built
`UBOOT.BIN` from USB into RAM and jump into it directly:

```
=> usb start
=> fatload usb 0:1 0x1000000 UBOOT.BIN
=> go 0x1000000
```

`go <addr>` (already in this build's command table) starts executing
raw code at an arbitrary RAM address — no flashing involved.

Three things specific to this board are worth being careful about before
trusting this, none of them blocking in principle but all untested:

1. **The ARK header vs. `go`'s entry point.** Normally `Stepldr` reads
   the ARK header's `EP` field (see `docs/uboot_build.md` §"Stock Binary
   Structure") and jumps straight to `board_init_r`, skipping the
   exception vector table. `go` doesn't know about that header — it
   starts at the very first byte (the reset vector, which branches to
   `_start`). This should still be safe: `CONFIG_SKIP_LOWLEVEL_INIT` —
   the flag that stops U-Boot from re-running DDR init and hanging
   (originally discovered the hard way, see `docs/uboot_build.md`
   "Problem 3") — is compiled into the binary itself, not something only
   the header-jump shortcut provides. Going in via `_start` should skip
   DDR reinit the same way, just via the standard path instead of
   Stepldr's shortcut. Not yet verified on hardware.
2. **Memory placement.** The second copy's load address must not
   collide with the *first* (currently-executing) copy's live code/stack.
   `0x1000000` avoids the running copy's origin (`0x30000`) but hasn't
   been checked against exactly where the first copy relocates itself to
   at runtime.
3. **This is a warm handoff, not a real reset.** The second U-Boot
   re-runs all its own hardware init (clocks, GPIO, console, MMC/USB) on
   top of whatever state the first one already left behind, rather than
   starting from Stepldr's known-clean post-DDR-init state. Probably
   fine (close to what happens on every normal boot), but is new,
   untested territory for this SoC specifically — the DDR-reinit
   sensitivity was a real, previously-hit hang on this board (the
   original "Starting Uboot → no console" failure mode during initial
   bring-up), so this deserves a cautious first test with serial
   watched closely and a readiness to power-cycle rather than an
   assumption that it's silent and safe.

Not yet implemented or tested. If it works, a wrapper command (e.g.
`usbuboot`, combining the fatload+go steps with basic sanity checks)
would be a reasonable next step to make it a one-liner.

**CORRECTION (verified via objdump disassembly of the real Stepldr.bin,
Holden firmware update package):** point 1 above was wrong. Stepldr does
NOT read the ARK header's `EP` field and jump to `board_init_r`. Its
actual load routine hardcodes `mov r0, #0x30000` immediately followed by
`blx r0` — it jumps straight to the fixed load address (the reset
vector / `_start`), the same as `go 0x30000` would, ignoring the header's
`EP` field entirely. (`EP` may be used for something else — validation,
bookkeeping — not confirmed.)

This matters directly for `bootstock` (`ark1668_boot_cmds.c`), which was
built on the wrong assumption and has been jumping to the header's `EP`
(`0x54ef8`) instead of the load address (`0x30000`) — skipping whatever
the stock binary's own `_start`/vector-table setup does. That's a
plausible explanation for `bootstock`'s intermittent `undefined instr
resetting` crashes (worked once, failed consistently after — skipping
required low-level init would produce exactly that kind of "usually
fine, occasionally traps" pattern). Fix: change `bootstock`'s `go`
target from the header `EP` to `STOCK_UBOOT_LOAD_ADDR` (`0x30000`)
directly. Not yet applied/tested as of this writing — see session notes.
