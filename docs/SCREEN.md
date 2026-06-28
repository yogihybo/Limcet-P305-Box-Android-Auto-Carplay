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

To flash userdata only via SD card, select item 2 in `build.sh` and
run:

```bash
bash build.sh
# Select: 2 (User Data) only, then g to generate
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
