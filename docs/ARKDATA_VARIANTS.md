# Arkdata Variant Reference

Panel display configuration presets bundled in the rootfs at
`msnprofile/arkdata/`. At boot, `MsnCoreApp` reads `ScreenType` and
`ResolutionType` from `MsnProductInfo.ini` and selects the matching
preset. Each file contains the full LCD timing, clock divider, and
touch-key configuration for one panel type.

## Naming convention

`arkdataNN_X.ini` — `NN` is a sequential panel ID; `X` is the hardware
platform family letter (corresponding broadly to the `ResourceName` in
`MsnProductInfo.ini`, e.g. Box-P → group P, Box-V → group V).

## Screen type key (ScreenType field)

| Value | Interface |
|-------|-----------|
| 2 | RGB888 |
| 4 | LVDS |

## TvoutType key

| Value | Meaning |
|-------|---------|
| 1 | COMPOSITE |
| 12 | CVBS_NTSC |

---

## Group A — arkdata1–4

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata1_A | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" LVDS |
| arkdata2_A | 400×240 | 8 | 18 | 40 | 7 | 43 | Small screen LVDS |
| arkdata3_A | 400×240 | 6 | 3 | 22 | 7 | 7 | Small screen LVDS, alternate timing |
| arkdata4_A | 800×480 | 11 | 9 | 50 | 10 | 35 | Identical to arkdata1_A |

## Group B — arkdata5–8

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata5_B | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" LVDS |
| arkdata6_B | 800×480 | 11 | 9 | 50 | 10 | 35 | Identical to arkdata5_B |
| arkdata7_B | 960×540 | 11 | 6 | 32 | 4 | 8 | Widescreen LVDS |
| arkdata8_B | 1440×540 | 7 | 12 | 139 | 10 | 14 | Ultra-wide RGB888 |

## Group C — arkdata9–10

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata9_C | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" LVDS |
| arkdata10_C | 800×480 | 11 | 1 | 50 | 6 | 35 | Alternate VBP/VSW |

## Group D — arkdata13–15

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata13_D | 1280×480 | 8 | 4 | 9 | 4 | 15 | Wide-format LVDS |
| arkdata14_D | 800×480 | 11 | 13 | 86 | 19 | 17 | RGB888, high HBP |
| arkdata15_D | 1280×480 | 8 | 4 | 5 | 4 | 9 | Wide-format LVDS, alternate HBP |

## Group E — arkdata17–20

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata17_E | 1280×480 | 8 | 4 | 9 | 4 | 15 | Wide-format LVDS |
| arkdata18_E | 800×480 | 11 | 19 | 132 | 12 | 20 | LVDS, high HBP |
| arkdata19_E | 1280×480 | 8 | 4 | 5 | 4 | 9 | Wide-format LVDS, alternate HBP |
| arkdata20_E | 800×480 | 11 | 19 | 143 | 12 | 20 | LVDS, higher HBP variant |

## Group F — arkdata21–23

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata21_F | 800×480 | 11 | 35 | 40 | 5 | 34 | LVDS, high VBP |
| arkdata22_F | 400×240 | 8 | 18 | 40 | 7 | 117 | Small screen LVDS |
| arkdata23_F | 1024×480 | 10 | 4 | 9 | 4 | 15 | Wide 7" LVDS |

## Group G — arkdata25–26

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata25_G | 400×240 | 8 | 18 | 40 | 7 | 43 | Small screen LVDS |
| arkdata26_G | 800×480 | 11 | 22 | 25 | 8 | 31 | LVDS, low HBP |

## Group H — arkdata29–31

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata29_H | 800×480 | 11 | 35 | 40 | 5 | 34 | LVDS, high VBP |
| arkdata30_H | 800×480 | 11 | 28 | 40 | 5 | 34 | LVDS, lower VBP variant |
| arkdata31_H | 400×240 | 8 | 10 | 40 | 7 | 43 | Small screen LVDS |

## Group I — arkdata33–36

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata33_I | 800×480 | 11 | 35 | 40 | 5 | 34 | LVDS, high VBP |
| arkdata34_I | 400×240 | 8 | 18 | 40 | 7 | 43 | Small screen LVDS |
| arkdata35_I | 400×240 | 8 | 10 | 40 | 7 | 43 | Small screen LVDS, lower VBP |
| arkdata36_I | 800×480 | 11 | 28 | 40 | 5 | 34 | LVDS, lower VBP variant |

## Group J — arkdata37–39

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata37_J | 800×480 | 11 | 35 | 40 | 5 | 34 | LVDS, high VBP |
| arkdata38_J | 400×240 | 8 | 13 | 40 | 7 | 43 | Small screen LVDS |
| arkdata39_J | 800×480 | 11 | 26 | 40 | 5 | 34 | LVDS, lower VBP |

## Group K — arkdata41–42

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata41_K | 800×480 | 11 | 35 | 40 | 5 | 34 | LVDS, high VBP |
| arkdata42_K | 400×240 | 8 | 18 | 40 | 7 | 43 | Small screen LVDS |

## Group L — arkdata45–46

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata45_L | 400×240 | 8 | 18 | 40 | 7 | 43 | Small screen LVDS |
| arkdata46_L | 400×240 | 8 | 13 | 40 | 7 | 43 | Small screen LVDS, lower VBP |

## Group M — arkdata49–50

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata49_M | 800×480 | 11 | 14 | 50 | 18 | 35 | LVDS, higher VSW |
| arkdata50_M | 800×480 | 11 | 14 | 50 | 18 | 35 | Identical, TvoutType=1 |

## Group N — arkdata53–55

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata53_N | 800×480 | 10 | 16 | 102 | 14 | 113 | LVDS, CLKDIV1=10, high HBP/HSW |
| arkdata54_N | 800×480 | 10 | 13 | 102 | 14 | 113 | As above, lower VBP |
| arkdata55_N | 1280×480 | 10 | 16 | 102 | 14 | 113 | Wide variant of arkdata53_N |

## Group O — arkdata57–60

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata57_O | 800×480 | 11 | 13 | 50 | 13 | 14 | LVDS |
| arkdata58_O | 800×480 | 11 | 13 | 50 | 13 | 14 | Identical to arkdata57_O |
| arkdata59_O | 800×480 | 9 | 16 | 72 | 18 | 61 | CLKDIV1=9, high HFP (267) |
| arkdata60-O | 800×480 | 11 | 25 | 109 | 12 | 21 | High HBP, TvoutType=1 |

## Group P — arkdata61–64

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata61_P | 800×480 | 11 | 6 | 36 | 22 | 36 | LVDS, high VSW |
| arkdata62_P | 400×240 | 6 | 3 | 22 | 7 | 7 | Small screen, CLKDIV1=6 |
| arkdata63_P | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" timing |
| arkdata64_P | 400×240 | 6 | 3 | 22 | 7 | 7 | Identical to arkdata62_P |

## Group Q — arkdata65–68

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata65_Q | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" LVDS |
| arkdata66_Q | 800×480 | 11 | 9 | 50 | 10 | 35 | Identical to arkdata65_Q |
| arkdata67_Q | 960×540 | 11 | 6 | 32 | 4 | 8 | Widescreen LVDS (same as arkdata7_B) |
| arkdata68_Q | 1440×540 | 7 | 12 | 139 | 10 | 14 | Ultra-wide RGB888 (same as arkdata8_B) |

## Group R — arkdata69–74

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata69_R | 800×480 | 11 | 9 | 50 | 10 | 35 | Standard 7" LVDS |
| arkdata70_R | 800×480 | 11 | 1 | 50 | 6 | 35 | Very low VBP variant |
| arkdata73_R | 800×480 | 11 | 9 | 50 | 10 | 35 | Identical to arkdata69_R |
| arkdata74_R | 800×480 | 11 | 1 | 50 | 6 | 35 | Identical to arkdata70_R |

## Group S — arkdata81–84

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata81_S | 640×240 | 11 | 3 | 3 | 3 | 12 | Narrow screen LVDS |
| arkdata82_S | 400×240 | 7 | 11 | 16 | 7 | 30 | Small screen, very high HFP (514) |
| arkdata83_S | 400×240 | 7 | 10 | 8 | 7 | 7 | As above, lower HBP |
| arkdata84_S | 640×240 | 11 | 3 | 3 | 3 | 12 | Narrow screen, higher HFP (200) |

## Group T — arkdata89–92

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata89_T | 800×480 | 10 | 13 | 177 | 21 | 43 | CLKDIV1=10, very high HBP |
| arkdata90_T | 800×480 | 10 | 33 | 177 | 21 | 43 | As above, higher VBP |
| arkdata91_T | 800×480 | 10 | 32 | 171 | 21 | 43 | As above, slightly lower HBP |
| arkdata92_T | 800×480 | 10 | 24 | 90 | 21 | 43 | Lower HBP variant |

## Group U — arkdata97

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata97_U | 800×480 | 11 | 29 | 166 | 12 | 49 | LVDS, high HBP |

## Group V — arkdata105–109 ⭐ Prado panel family

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata105_V | 800×480 | 10 | 33 | 215 | 21 | 30 | High HBP, CLKDIV1=10 |
| arkdata106_V | 800×480 | 11 | 29 | 32 | 16 | 54 | **Prado / Limcet-P306 panel** |
| arkdata107_V | 800×480 | 11 | 29 | 32 | 16 | 54 | Identical to arkdata106_V (alternate supplier) |
| arkdata108_V | 800×480 | 10 | 35 | 235 | 21 | 30 | Very high HBP, CLKDIV1=10 |
| arkdata109_V | 800×480 | 9 | 33 | 100 | 1 | 127 | LVDS cfg=0xE0EC — likely LVDS variant |

> **arkdata106_V / arkdata107_V** exactly match the `mtd4_arkdata` on the live Prado device
> (`CLKDIV1=11`, `VBP=29`, `HBP=32`, `VFP=25`, `HSW=54`). These two entries are the same
> timing for two different panel suppliers.

## Group W — arkdata113–114

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata113_W | 800×480 | 10 | 16 | 102 | 14 | 113 | Same timing as arkdata53_N |
| arkdata114_W | 800×480 | 10 | 16 | 102 | 14 | 113 | Identical to arkdata113_W |

## Group X — arkdata121–124

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata121_X | 800×480 | 10 | 29 | 130 | 14 | 113 | CLKDIV1=10, moderate HBP |
| arkdata122_X | 800×480 | 10 | 18 | 130 | 14 | 113 | Lower VBP variant |
| arkdata123_X | 800×480 | 10 | 29 | 130 | 14 | 68 | Lower HSW variant |
| arkdata124_X | 800×480 | 10 | 29 | 130 | 10 | 78 | Lower VSW/alternate HSW |

## Group Y — arkdata129–131

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata129_Y | 800×480 | 11 | 19 | 132 | 12 | 20 | Same as arkdata18_E |
| arkdata130_Y | 1280×480 | 8 | 4 | 9 | 4 | 15 | Wide-format (same as arkdata13_D) |
| arkdata131_Y | 1440×540 | 6 | 27 | 130 | 3 | 22 | Ultra-wide RGB888, different timing to 8_B |

## Group Z — arkdata137

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata137_Z | 800×480 | 12 | 9 | 48 | 10 | 35 | CLKDIV1=12 — fastest pixel clock in library |

## Group AA — arkdata140

| File | Resolution | CLKDIV1 | VBP | HBP | VSW | HSW | Notes |
|------|-----------|---------|-----|-----|-----|-----|-------|
| arkdata140_AA | 1280×480 | 10 | 40 | 102 | 14 | 113 | Wide-format, high VBP |

---

## Holden firmware update package variants

These files ship with `HOLDEN_KS_Auto_DSP(BT)_0219` and are not in the
Prado panel library above.

| File | Resolution | CLKDIV1 | VBP | HBP | TouchKeys | Notes |
|------|-----------|---------|-----|-----|-----------|-------|
| arkdata.ini | 800×480 | 10 | 3 | 20 | 5 | Holden Commodore/Cruze panel — **causes screen hue on Prado** |
| arkdata0324.ini | 800×480 | 10 | 3 | 20 | 5 | March 2024 update — minor VP tweak (video2Brightness/Hue) |
| arkdata君威.ini | 480×240 | 6 | 6 | 32 | 0 | Buick LaCrosse (君威) — Chinese market, small portrait screen |

---

## Special: msnprofile/arkdata.ini (live Prado dump)

This file sits at `msnprofile/arkdata.ini` (not in the `arkdata/` subfolder)
and is loaded directly rather than selected by ID.

| Resolution | ScreenType | CLKDIV1 | VBP | HBP | LVDSCfg | Notes |
|-----------|-----------|---------|-----|-----|---------|-------|
| 1024×600 | 4 (LVDS) | 7 | 8 | 50 | 0x160FD | LVDS premium screen — not the Prado panel |

This preset is for a different vehicle (likely Buick Enclave or similar with
an LVDS 1024×600 display). It was present in the Box-P301 base firmware.
The Prado application ignores it because `MsnProductInfo.ini` declares
`ScreenType=1` (RGB), directing the app to the RGB rendering path using the
mtd4 hardware timings instead.

---

## Resolution summary

| Resolution | Interface | Typical application |
|-----------|-----------|---------------------|
| 400×240 | LVDS | Budget/older small-screen HUs (pre-2015) |
| 480×240 | LVDS | Buick LaCrosse (China market) |
| 640×240 | LVDS | Narrow instrument-cluster style screens |
| 800×480 | LVDS/RGB888 | Standard 7"–8" aftermarket HUs — most common |
| 960×540 | LVDS | Widescreen fitments |
| 1024×480 | LVDS | Wide 7" panel variant |
| 1024×600 | LVDS | Premium OEM LVDS (Buick Enclave, etc.) |
| 1280×480 | LVDS | Double-DIN ultra-wide panels |
| 1440×540 | RGB888 | Ultra-wide, newer panel technology |
