# Screen Configuration & Hue Investigation

## Hardware

The Prado head unit uses a direct **RGB888 parallel panel**, 800×480.

| Parameter | Value |
|-----------|-------|
| Interface | RGB888 (parallel) |
| Resolution | 800×480 |
| CLKDIV1 | 11 |
| VBP / HBP | 29 / 32 |
| VFP / HFP | 25 / 25 |
| VSW / HSW | 16 / 54 |
| TvoutType (mtd4) | 1 (COMPOSITE) |
| RgbMode | 0 (BGR) |
| Format | 7 (RGB_888) |
| Matching arkdata preset | `arkdata106_V` / `arkdata107_V` |

---

## Display configuration layers

The ARK1680 platform applies display settings in two distinct stages:

### Stage 1 — U-Boot hardware init (mtd4 arkdata)

At power-on, U-Boot reads `arkdata.ini` from the **mtd4 partition** and
programs the display controller hardware registers directly — timings,
clock dividers, pixel format, and TvoutType. This sets the physical
panel signal and cannot be overridden without reflashing mtd4.

### Stage 2 — Application colour pipeline (MsnCoreApp)

After the kernel boots, `MsnCoreApp` reads two further config sources:

1. **`MsnProductInfo.ini`** (from userdata `msncfg/`) — `ScreenType`
   field selects which colour output pipeline the app uses for all
   rendered content written to the framebuffer.
2. **`msnprofile/arkdata.ini`** (from rootfs) — loaded at runtime; may
   reprogram the ARK1680 video processor (VP) colour matrix and
   TvoutType via driver ioctls.

These two stages are independent. Correct hardware timings at Stage 1
do not prevent colour corruption if Stage 2 uses the wrong pipeline.

---

## ScreenType in MsnProductInfo

`ScreenType` in `MsnProductInfo.ini` uses a **different enum** to the
`ScreenType` field in `arkdata.ini`. In the arkdata context,
`ScreenType` describes the physical panel interface (RGB565=1,
RGB888=2, LVDS=4...). In the `MsnProductInfo` context it selects the
application-level colour output pipeline:

| Value | Pipeline | Description |
|-------|----------|-------------|
| 1 | Direct RGB | App renders straight BGR888 to framebuffer — correct for Prado |
| 3 | CVBS / ITU656 | App encodes output through YUV/CVBS colour matrix — for composite-fed displays |

The Holden unit (`Box-C211`) uses `ScreenType=3`, suggesting its
display is driven via a composite/ITU656 video path rather than a
direct RGB connection — a common arrangement in some vehicle-specific
OEM fitments.

---

## Screen hue when running Holden firmware

### Background

The Holden firmware update (`HOLDEN_KS_Auto_DSP(BT)_0219`) was
flashed to the Prado device and booted successfully. A visible screen
hue was observed. Initial suspicion was the Holden `arkdata.ini` LCD
timings, but the boot log disproved this.

### Boot log finding — arkdata was NOT flashed

The SD update script included `arkdata.ini` but the boot log shows:

```
** Unable to read "arkdata.ini" from mmc 0:1 **
```

U-Boot could not read it from the SD card and fell back to the
existing NAND content — the **Prado's own arkdata** (6,671 bytes,
CLKDIV1=11, VBP=29, HBP=32). The hardware display timing was never
changed. The original assumption (timing mismatch = hue) was incorrect.

### Root cause — two compounding issues

#### Issue 1: MsnProductInfo.ScreenType=3 (primary cause)

The Holden `userdata.img` was successfully flashed, overwriting the
Prado's `msncfg/MsnProductInfo.ini` with the Holden version:

| | Prado | Holden |
|--|-------|--------|
| `ScreenType` | **1** (direct RGB) | **3** (CVBS/ITU656) |
| `ProductId` | Limcet-P306 | Ksmart_DSP |
| `McuType` | 6 | 16 |
| `SoundType` | 0 | 4 (DSP) |

With `ScreenType=3`, `MsnCoreApp` routes all rendered frames through a
YUV/CVBS colour encoder pipeline. The Prado panel expects straight
BGR888. Receiving YUV-encoded data remaps the colour channels,
producing the observed hue shift.

This is confirmed by the boot log sequence: U-Boot correctly
initialises the hardware as `screen_type=0` (RGB), but the hue appears
**after the app starts** — not at the boot screen stage — consistent
with the application-level colour pipeline being the cause.

#### Issue 2: msnprofile/arkdata TvoutType=12 (contributing factor)

The Holden rootfs contains `msnprofile/arkdata.ini` with
`TvoutType=12` (CVBS_NTSC) and Holden-specific LCD timings. At
runtime, `MsnCoreApp` may reprogram the ARK1680 video processor colour
matrix via this file. Under `ScreenType=3`, the app may act on the
msnprofile arkdata's `TvoutType=12` and switch the VP colour matrix to
NTSC encoding. A CVBS_NTSC colour matrix applied to a direct RGB panel
produces a pronounced hue shift.

The Prado's own `msnprofile/arkdata.ini` also has `TvoutType=12`, but
with `ScreenType=1` the app ignores the msnprofile arkdata's colour
settings and uses the direct RGB path — so on original Prado firmware
this causes no problem.

### Why the original Prado firmware has correct colours

| Config source | Prado firmware | Holden firmware (on Prado) |
|---------------|---------------|---------------------------|
| mtd4 arkdata | CLKDIV1=11, VBP=29, TvoutType=1 | Same (not updated) |
| MsnProductInfo.ScreenType | **1** → direct RGB | **3** → CVBS/YUV path |
| msnprofile/arkdata TvoutType | 12 (ignored — ScreenType=1) | 12 (applied — ScreenType=3) |
| VP colour matrix | RGB passthrough | NTSC YUV encoding |
| Result | Correct colours | **Hue shift** |

### Fix

Flash the **Prado userdata** to restore `MsnProductInfo.ScreenType=1`.
No change to rootfs, kernel, or mtd4 arkdata is required. The
reconstructed `userdata.img` already contains the correct
`MsnProductInfo.ini`.

To flash userdata only via SD card, run `build_update.sh`, deselect
everything except User Data (mtd7), then generate:

```bash
bash build_update.sh
# n to deselect all, arrow keys + Space to select only User Data, then g to generate
```

Copy `sd_update/output/` to a FAT32 SD card and power on.

---

## msnprofile/arkdata.ini on the Prado dump — unexpected content

The `msnprofile/arkdata.ini` in the live Prado dump (inside the rootfs,
not the mtd4 partition) contains:

| Field | Value |
|-------|-------|
| ScreenId | 6 |
| ScreenType | 4 (LVDS) |
| Resolution | 1024×600 |
| LVDSCfg | 0x160FD |
| TvoutType | 12 |

This is a profile for a **1024×600 LVDS panel** — not the Prado
screen. It was present in the Box-P301 base firmware, which is shared
with other vehicles that use an LVDS display (likely Buick Enclave or
similar). The Prado application ignores it because
`MsnProductInfo.ScreenType=1` directs the app to the direct RGB
rendering path.

See [`ARKDATA_VARIANTS.md`](ARKDATA_VARIANTS.md) for the full panel
preset library reference.

---

## Panel/model selection mechanism & the DIP switch (2026-07-11)

Investigated how the unit selects LCD settings for different vehicle models
(there is a physical DIP switch on the board that appears to change the panel).
Traced across the dumped U-Boot, the stock 3.4.0 kernel (`vmlinux.elf`
disassembly), and the userspace app.

### How a panel is selected — the `screen` id

The selected panel is indexed by a single **screen id**:

- **Kernel:** the `screen=N` boot argument sets `g_screen_id`, which indexes a
  built-in panel table — `screens[g_screen_id]`. Confirmed two ways: (1) the
  vendor reference source
  [`../ArkPro Reference/kernel/drivers/ark/display/ark_display_core.c`](../ArkPro%20Reference/kernel/drivers/ark/display/ark_display_core.c)
  (`screen_id_setup` → `__setup("screen=")`, `struct screen_info *screen =
  &screens[g_screen_id]`); and (2) disassembly of the dumped kernel's own
  `screen_id_setup` (`vmlinux.elf`), which `memcpy`s a 120-byte `screen_info`
  struct into the exported global `screeninfo_param`. Named panels in the enum:
  `SCREEN_QUN700`, `SCREEN_CVBS_NTSC/PAL`, `SCREEN_VGA8060`, `SCREEN_YPBPR720P`,
  `SCREEN_C101EAN`, `SCREEN_CLAA101`, `SCREEN_GM8284DD`.
- **U-Boot:** the same `screen` value (env var) selects which `ScreenId` block of
  **mtd4 `arkdata.ini`** to program into the display-controller registers. This is
  the *authoritative* timing source at Stage 1 (see "Display configuration layers"
  above); the kernel's built-in `screens[]` entry is only a fallback the app/driver
  can re-apply later. In the dumped unit `arkdata.ini` has a single `ScreenId=0`
  (800×480 RGB888), and the env has a static `screen=0`.

So "which panel" is decided by the **`screen` value**, consumed identically by
U-Boot (arkdata `ScreenId`) and the kernel (`screens[]` index).

### The DIP switch

- There is **no `dip`/`dipswitch` string** anywhere in U-Boot, the kernel, or the
  rootfs — the switch is read as raw **GPIO strapping**, not by that name.
- The **vendor reference BSP selects the screen purely from the `screen` env var**
  — it contains **no** GPIO/DIP read. So a DIP-driven panel change is an **OEM
  customisation** on this board.
- The **userspace app does not read the DIP**: `MsnCoreApp` takes screen/resolution
  from the static `MsnProductInfo.ini` (`ResourceName=Box-P301`, `ScreenType=1`,
  `ResolutionType=1`); `libMsnCommons` only exposes a generic
  `/sys/class/gpio/gpio%d/{direction,value,edge}` helper, with no model mapping.
- Therefore the DIP is almost certainly read by the **OEM U-Boot** and used to set
  `screen`/`ScreenId` before boot. The dumped U-Boot has the matching machinery
  (`ScreenId`, `SubScreenType`, `disconfig ${screen}`,
  `get screenInfo … set default screen_id = %d`). **Not yet byte-confirmed:** the
  exact GPIO(s) the OEM U-Boot reads — would require disassembling the dumped
  U-Boot's screen-select path (no symbols, so more involved than the kernel).

### Evidence the unit is genuinely multi-panel

- Rootfs ships Launcher resources at **two resolutions**: `Launcher-*-800x480.rcc`
  and `Launcher-*-1024x600.rcc`.
- The rootfs `msnprofile/arkdata.ini` (distinct from mtd4) is a **1024×600 LVDS**
  profile (`ScreenId=6`, `ScreenType=4`, `LVDSCfg=0x160FD`) — a sibling-vehicle
  panel the Prado app ignores (see section above).
- `ARKDATA_VARIANTS.md` catalogues 30+ presets (800×480, 400×240, 960×540,
  1280×480, 1024×480, LVDS/RGB888) selectable by this same id scheme.

**Bottom line:** panel selection is a single `screen` id flowing U-Boot→kernel; the
DIP switch is an OEM GPIO strap that (almost certainly) sets that id in the OEM
U-Boot. Confirming the exact GPIO mapping is the one remaining open item.
