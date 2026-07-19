#!/bin/sh
# lcd-blend-sweep.sh -- consolidated live register test plan for the OSD1
# per-pixel-alpha-blend hue-corruption bug (docs/DEVICE_TEST_CHECKLIST_2026-07-18.md
# section 1b).
#
# Background so far (2026-07-19 session):
#   - Fully-opaque pixels (alpha=255) always render correctly, on every
#     register combination tried. Only PARTIAL-alpha pixels are wrong.
#   - blend_mode (MODE_LCD_REG0 bits[15:12], LCDC base+0x60) defaults to 0
#     (broken) in our driver; stock's own struct-init default is 1.
#     Empirically swept 0-15 on hardware: modes 9, 10, and 14 all turn on
#     real per-pixel alpha blending (6 distinct fb-alpha-test bands instead
#     of 4 merged ones) -- but the color is WRONG for every partial-alpha
#     band once blending is active.
#   - rgb_order (OSD1_CTL bits[20:18], LCDC base+0x74) swept 0-7 with
#     blend_mode=9: band 2 (half-alpha red) came out dark-green(0,1),
#     light-green(2,3), brown(4,5), dark-green(6,7) -- NEVER a correct
#     dim red. rgb_order alone does not fix it.
#   - format (OSD1_CTL bits[15:12]) -- NOT YET TESTED empirically.
#   - Y2R_COEF321/654/7 (LCDC base+0x11c/0x120/0x124) -- YCbCr->RGB matrix
#     coefficients our driver writes unconditionally even though OSD1 is
#     pure RGB. Hypothesis: the blend/compositor path routes partial-alpha
#     pixels through this matrix even when the "bypass" bits say not to,
#     which would explain unpredictable hue shifts that vary with
#     rgb_order (different RGB permutations hitting a fixed YCbCr matrix
#     produce different, non-obvious output hues) while sparing opaque
#     pixels (which may use a separate, simpler passthrough path). NOT YET
#     TESTED.
#   - ALPHA1_0_VIDEO_OSD1 (LCDC base+0x24) -- a completely separate,
#     never-touched alpha/blend-weight register found in the register
#     header. Could not identify what stock's kernel writes here via
#     Ghidra in the time available. NOT YET TESTED.
#
# This script walks through the untested candidates in order of how
# promising they seem, pausing after each for you to run fb-alpha-test and
# report back what band 1 (opaque red, should stay pure red) and band 2
# (half-alpha red, should become a DIM/MUTED red, not a different hue)
# look like.
#
# Usage: sh lcd-blend-sweep.sh

LCDC=0xe0500000
MODE_REG0=$((LCDC + 0x60))
OSD1_CTL=$((LCDC + 0x74))
ALPHA1_0_VIDEO_OSD1=$((LCDC + 0x24))
Y2R_COEF321=$((LCDC + 0x11c))
Y2R_COEF654=$((LCDC + 0x120))
Y2R_COEF7=$((LCDC + 0x124))

pause() {
    echo "$1"
    echo "Press Enter to continue..."
    read _
}

echo "=== lcd-blend-sweep.sh starting ==="
killall MsnCoreApp 2>/dev/null

echo
echo "--- Baseline register dump (before any changes) ---"
printf "MODE_LCD_REG0 (blend_mode):        "; devmem $MODE_REG0 32
printf "OSD1_CTL (format/alpha/rgb_order):  "; devmem $OSD1_CTL 32
printf "ALPHA1_0_VIDEO_OSD1 (untested):     "; devmem $ALPHA1_0_VIDEO_OSD1 32
printf "Y2R_COEF321:                        "; devmem $Y2R_COEF321 32
printf "Y2R_COEF654:                        "; devmem $Y2R_COEF654 32
printf "Y2R_COEF7:                          "; devmem $Y2R_COEF7 32

echo
echo "--- Locking in blend_mode=9 (confirmed to enable real per-pixel alpha) ---"
base=$(devmem $MODE_REG0 32)
devmem $MODE_REG0 32 $(( (base & 0xFFFF0FFF) | (9 << 12) ))

echo
echo "=== PHASE 1: sweep 'format' field (OSD1_CTL bits[15:12]), rgb_order held at 0 ==="
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 $(( base & ~(0x7 << 18) ))   # rgb_order=0

for fmt in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    base=$(devmem $OSD1_CTL 32)
    newval=$(( (base & ~(0xF << 12)) | (fmt << 12) ))
    printf "\n=== format=%d  (OSD1_CTL=0x%08x) ===\n" "$fmt" "$newval"
    devmem $OSD1_CTL 32 "$newval"
    fb-alpha-test >/dev/null
    pause "Band 1 (opaque red) and Band 2 (half-alpha red) -- report both colors."
done

echo
echo "=== PHASE 2: Y2R_COEF zero test (format/rgb_order back to known baseline: format=6, rgb_order=0) ==="
base=$(devmem $OSD1_CTL 32)
newval=$(( (base & ~(0xF << 12) & ~(0x7 << 18)) | (6 << 12) ))
devmem $OSD1_CTL 32 "$newval"

echo "Y2R_COEF values before zeroing:"
devmem $Y2R_COEF321 32
devmem $Y2R_COEF654 32
devmem $Y2R_COEF7 32
devmem $Y2R_COEF321 32 0
devmem $Y2R_COEF654 32 0
devmem $Y2R_COEF7 32 0
fb-alpha-test >/dev/null
pause "Band 2 -- did zeroing Y2R_COEF change its color AT ALL from the green/brown seen before?"

echo
echo "=== PHASE 3: ALPHA1_0_VIDEO_OSD1 sweep (never touched before) ==="
for val in 0x00000000 0x000000ff 0x0000ff00 0x00ff0000 0xff000000 0x000080ff 0x0080ff00; do
    printf "\n=== ALPHA1_0_VIDEO_OSD1=%s ===\n" "$val"
    devmem $ALPHA1_0_VIDEO_OSD1 32 "$val"
    fb-alpha-test >/dev/null
    pause "Band 1 and Band 2 -- report both colors."
done

echo
echo "=== Sweep complete. Restoring known-safe defaults (format=6, rgb_order=0, blend_mode=9) before exiting. ==="
devmem $ALPHA1_0_VIDEO_OSD1 32 0
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 $(( (base & ~(0xF << 12) & ~(0x7 << 18)) | (6 << 12) ))
echo "Done -- report results for phases 1-3."
