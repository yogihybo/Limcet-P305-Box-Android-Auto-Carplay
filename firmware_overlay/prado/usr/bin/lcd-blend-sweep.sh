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
#     band once blending is active. Mode 1 (stock's default) does NOT by
#     itself turn on visible blending on our reconstruction, which is
#     itself unexplained.
#   - rgb_order (OSD1_CTL bits[20:18], a 3-bit field -- confirmed correct
#     via a kernel debug-proc help string found in stock's vmlinux.elf
#     strings: "rgb_order: 0=rgb, 1=rbg, 2=grb, 3=gbr, 4=brg, 5=bgr").
#     ALREADY SWEPT 0-7 (covering all 6 meaningful values) with
#     blend_mode=9: band 2 (half-alpha red) came out dark-green (0,1),
#     light-green (2,3), brown (4,5), dark-green (6,7) -- NEVER a correct
#     dim red. rgb_order is EXHAUSTED as a standalone fix.
#     (An earlier revision of this file incorrectly claimed rgb_order was
#     only a 2-bit field at bits[22:21] and had never been tested -- that
#     was wrong, based on an unverified Ghidra parameter-order guess that
#     was reverted in linux-arkmicro 926336ce7 once the debug string
#     above was found. Sorry for the churn if you're reading this after
#     already running the old version.)
#   - yuv_order (OSD1_CTL bits[22:21], 2-bit field) -- NOT YET
#     independently swept (only ever changed alongside rgb_order in a
#     combined test). Low priority: this is a pure-RGB layer, yuv_order
#     shouldn't matter for RGBA888, but worth ruling out empirically
#     since nothing else has worked.
#   - format (OSD1_CTL bits[15:12]) -- confirmed via Ghidra to be a direct
#     passthrough (the raw format value gets shifted into place unchanged,
#     no remapping), so our current value (6 = RGBA888) is very likely
#     already correct. LOW PRIORITY.
#   - Y2R_COEF321/654/7 (LCDC base+0x11c/0x120/0x124) -- RULED OUT. Found
#     stock's ark_disp_set_lcd_panel_type() (@ 0x802e0a78) hardcoding the
#     EXACT SAME literals our driver already writes (COEF321=0x1a916d2a,
#     COEF654=0x1d12e060, COEF7 ORs in 0x1029) -- confirmed via direct
#     calculation, byte-for-byte match. Not the cause.
#   - ALPHA1_0_VIDEO_OSD1 (LCDC base+0x24) -- RULED OUT. Systematically
#     traced every access to the LCDC MMIO base across the entire
#     display-driver code range and found zero writes to this offset.
#     Very likely unused for this board.
#
# CURRENT STATE: every register-level candidate found so far is either
# ruled out or exhausted without producing a correct result. This script
# now focuses on: (1) the one still-untested field (yuv_order alone),
# (2) re-verifying blend_mode=1 (stock's real default) actually latches
# by doing an explicit re-write rather than trusting the reset value,
# in case the compositor needs a write-triggered commit, not just a
# register holding the "right" value, (3) format, low priority.
#
# If none of this works, the likely next step is real reverse engineering
# of stock's actual userspace binaries (MsnCoreApp/libarkadapt.so/
# libarkcmn.so, all in firmware_dumps/Prado firmware dump/mtd6_rootfs/)
# to find whether a vendor ioctl call with a specific hardcoded payload
# is required at runtime that isn't replicated by any kernel-side
# default -- see tools/fb-alpha-test/README.md.
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
echo "=== PHASE 0: re-verify blend_mode=1 (stock's real default) with an EXPLICIT write-then-check ==="
echo "    Testing the hypothesis that the compositor needs a write-triggered commit, not just"
echo "    the register holding the right value at reset."
base=$(devmem $MODE_REG0 32)
devmem $MODE_REG0 32 $(( (base & 0xFFFF0FFF) | (1 << 12) ))
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 "$base"   # explicit re-write of the SAME value, to force a commit if one is needed
fb-alpha-test >/dev/null
pause "Bands 1/2 -- did an explicit re-write with blend_mode=1 change ANYTHING vs before?"

echo
echo "=== PHASE 1: sweep yuv_order alone (OSD1_CTL bits[22:21], 4 values) ==="
echo "    blend_mode locked to 9 (confirmed to enable real blending), rgb_order held at 0."
base=$(devmem $MODE_REG0 32)
devmem $MODE_REG0 32 $(( (base & 0xFFFF0FFF) | (9 << 12) ))
base=$(devmem $OSD1_CTL 32)
devmem $OSD1_CTL 32 $(( (base & ~(0x7 << 18) & ~(0x3 << 21) & ~(0xF << 12)) | (6 << 12) ))

for yuv in 0 1 2 3; do
    base=$(devmem $OSD1_CTL 32)
    newval=$(( (base & ~(0x3 << 21)) | (yuv << 21) ))
    printf "\n=== yuv_order=%d  (OSD1_CTL=0x%08x) ===\n" "$yuv" "$newval"
    devmem $OSD1_CTL 32 "$newval"
    fb-alpha-test >/dev/null
    pause "Band 2 -- report color."
done

echo
echo "=== PHASE 2 (low priority -- format is likely already correct): sweep 'format' field ==="
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
echo "Done -- report results."
