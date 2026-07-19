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
#   - CORRECTED FIELD LAYOUT (found via Ghidra, later same session): what
#     we thought was "rgb_order" (OSD1_CTL bits[20:18], swept 0-7 with
#     blend_mode=9, band 2 came out dark-green/light-green/brown/
#     dark-green -- never correct) was actually WRONG. Traced the real
#     parameter-passing convention in stock's ark_disp_set_osd_format(id,
#     format, yuv_order, rgb_order) (vmlinux.elf @ 0x802ddf98):
#       orr r2, r1, r7, lsl #18   -- r7 = yuv_order (3rd arg) -> bits[20:18]
#       orr r3, r2, r6, lsl #21   -- r6 = rgb_order (4th arg) -> bits[22:21]
#     rgb_order and yuv_order are SWAPPED from what this project assumed
#     earlier. The REAL rgb_order is only a 2-bit field (values 0-3) at
#     bits[22:21] -- it has NEVER been touched/swept. THIS IS THE MOST
#     PROMISING UNTESTED CANDIDATE. What we swept before (0-7 at
#     bits[20:18]) was actually yuv_order, a real but different field.
#   - format (OSD1_CTL bits[15:12]) -- confirmed via Ghidra to be a direct
#     passthrough (the raw format value gets shifted into place unchanged,
#     no remapping), so our current value (6 = RGBA888) is very likely
#     already correct assuming our format enum matches stock's -- it does,
#     since both come from the same vendor header lineage. LOW PRIORITY.
#   - Y2R_COEF321/654/7 (LCDC base+0x11c/0x120/0x124) -- RULED OUT. Found
#     stock's ark_disp_set_lcd_panel_type() (@ 0x802e0a78) hardcoding the
#     EXACT SAME literals our driver already writes (COEF321=0x1a916d2a,
#     COEF654=0x1d12e060, COEF7 ORs in 0x1029) -- confirmed via direct
#     calculation, byte-for-byte match. Not the cause.
#   - ALPHA1_0_VIDEO_OSD1 (LCDC base+0x24) -- RULED OUT. Systematically
#     traced every access to the LCDC MMIO base across the entire
#     display-driver code range and found zero writes to this offset.
#     ark_disp_set_layer_cfg() (the "apply full layer config" function)
#     calls a comprehensive list of per-layer setters and none target it.
#     Very likely unused for this board.
#
# This script now prioritizes the corrected rgb_order field (bits[22:21],
# only 4 possible values) as PHASE 1 -- this is the real untested
# candidate and the most likely fix. Format sweep is PHASE 2 (low
# priority, probably already correct). Y2R_COEF/ALPHA1_0_VIDEO_OSD1 are
# NOT included anymore since both were ruled out via Ghidra -- see
# tools/fb-alpha-test/README.md if you want to re-verify them anyway.
#
# Usage: sh lcd-blend-sweep.sh

LCDC=0xe0500000
MODE_REG0=$((LCDC + 0x60))
OSD1_CTL=$((LCDC + 0x74))

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
printf "OSD1_CTL (format/alpha/rgb_order/yuv_order): "; devmem $OSD1_CTL 32

echo
echo "--- Locking in blend_mode=9 (confirmed to enable real per-pixel alpha) ---"
base=$(devmem $MODE_REG0 32)
devmem $MODE_REG0 32 $(( (base & 0xFFFF0FFF) | (9 << 12) ))

echo
echo "=== PHASE 1 (MOST PROMISING): sweep the REAL rgb_order field ==="
echo "    (OSD1_CTL bits[22:21], only 4 values -- this was never tested before,"
echo "     what we tested last time as 'rgb_order' was actually yuv_order)"
echo "    yuv_order held fixed at 0, format held fixed at 6 (RGBA888)."
base=$(devmem $OSD1_CTL 32)
newval=$(( (base & ~(0xF << 12) & ~(0x7 << 18) & ~(0x3 << 21)) | (6 << 12) ))
devmem $OSD1_CTL 32 "$newval"

for order in 0 1 2 3; do
    base=$(devmem $OSD1_CTL 32)
    newval=$(( (base & ~(0x3 << 21)) | (order << 21) ))
    printf "\n=== REAL rgb_order=%d  (OSD1_CTL=0x%08x) ===\n" "$order" "$newval"
    devmem $OSD1_CTL 32 "$newval"
    fb-alpha-test >/dev/null
    pause "Band 1 (opaque red) and Band 2 (half-alpha red) -- report both colors. Looking for band 2 = DIM/MUTED red, not a different hue."
done

echo
echo "=== PHASE 1b: if none of the 4 rgb_order values alone worked, try combined with yuv_order too ==="
echo "    (16 combos: rgb_order 0-3 x yuv_order 0-3 -- only run this if phase 1 found nothing)"
for order in 0 1 2 3; do
    for yuv in 0 1 2 3; do
        base=$(devmem $OSD1_CTL 32)
        newval=$(( (base & ~(0x3 << 21) & ~(0x7 << 18)) | (order << 21) | (yuv << 18) ))
        printf "\n=== rgb_order=%d yuv_order=%d  (OSD1_CTL=0x%08x) ===\n" "$order" "$yuv" "$newval"
        devmem $OSD1_CTL 32 "$newval"
        fb-alpha-test >/dev/null
        pause "Band 2 -- report color."
    done
done

echo
echo "=== PHASE 2 (low priority -- format is likely already correct): sweep 'format' field ==="
echo "    rgb_order/yuv_order reset to 0 for this phase."
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 $(( base & ~(0x7 << 18) & ~(0x3 << 21) ))

for fmt in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    base=$(devmem $OSD1_CTL 32)
    newval=$(( (base & ~(0xF << 12)) | (fmt << 12) ))
    printf "\n=== format=%d  (OSD1_CTL=0x%08x) ===\n" "$fmt" "$newval"
    devmem $OSD1_CTL 32 "$newval"
    fb-alpha-test >/dev/null
    pause "Band 1 and Band 2 -- report both colors."
done

echo
echo "=== Sweep complete. Restoring known-safe defaults (format=6, rgb_order=0, yuv_order=0, blend_mode=9) before exiting. ==="
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 $(( (base & ~(0xF << 12) & ~(0x7 << 18) & ~(0x3 << 21)) | (6 << 12) ))
echo "Done -- report results, especially phase 1."
