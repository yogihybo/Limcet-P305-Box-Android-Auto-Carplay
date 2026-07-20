#!/bin/sh
# lcd-overlay-watch.sh -- live change-log of which LCDC layer/overlay is
# enabled and where each layer is pointing, for diagnosing the "red shade
# flashes on screen when using the input knob" bug (see
# docs/DEVICE_TEST_CHECKLIST_2026-07-18.md section 1b).
#
# Background: this SoC's LCDC has 5 real compositing layers (OSD1/2/3 +
# VIDEO1/2) plus a fallback "back color" fill shown wherever no layer
# covers a pixel. MsnCoreApp/DirectFB only ever paints OSD1 (/dev/fb0).
# OSD2 is otherwise only used by the (currently unused-at-runtime) JPEG
# boot-animation player; OSD3 and VIDEO1/2 have no known runtime user in
# this rootfs, and CONFIG_ARK_CARBACK / CONFIG_ARK1668_ITU656 are both
# unset in the current kernel .config, so VIDEO1/2 should never turn on
# at all right now. If any of that isn't true on the real hardware, this
# script will show it.
#
# It polls the layer-enable bits (ARK1668_LCDC_CONTROL, LCDC+0x04),
# the back-color fill (LCDC+0x50), and each layer's frame address
# register as fast as `devmem` allows, and only prints a line when
# something actually changes -- so it's safe to leave running while you
# mash the knob for a while, then Ctrl-C and send the output.
#
# Usage: lcd-overlay-watch.sh [seconds to run, default 30]

LCDC=0xe0500000
CONTROL=$((LCDC + 0x04))
BACK_COLOR=$((LCDC + 0x50))
OSD1_ADDR=$((LCDC + 0x80))
OSD2_ADDR=$((LCDC + 0x94))
OSD3_ADDR=$((LCDC + 0xa4))
VIDEO_ADDR1=$((LCDC + 0x54))
VIDEO2_ADDR1=$((LCDC + 0x338))
MODE_REG0=$((LCDC + 0x60))

DURATION="${1:-30}"

decode_control() {
    val="$1"
    v1=$(( (val >> 5) & 1 ))
    v2=$(( (val >> 6) & 1 ))
    o1=$(( (val >> 7) & 1 ))
    o2=$(( (val >> 8) & 1 ))
    o3=$(( (val >> 9) & 1 ))
    printf "VIDEO1=%d VIDEO2=%d OSD1=%d OSD2=%d OSD3=%d" "$v1" "$v2" "$o1" "$o2" "$o3"
}

echo "=== lcd-overlay-watch.sh: polling for ${DURATION}s, printing only on change ==="
echo "    (start this, then mash the knob until it stops, then Ctrl-C early is fine)"

prev_control=""
prev_back=""
prev_osd1=""
prev_osd2=""
prev_osd3=""
prev_vid1=""
prev_vid2=""
prev_prio=""

end=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$end" ]; do
    control=$(devmem $CONTROL 32)
    back=$(devmem $BACK_COLOR 32)
    osd1=$(devmem $OSD1_ADDR 32)
    osd2=$(devmem $OSD2_ADDR 32)
    osd3=$(devmem $OSD3_ADDR 32)
    vid1=$(devmem $VIDEO_ADDR1 32)
    vid2=$(devmem $VIDEO2_ADDR1 32)
    prio=$(devmem $MODE_REG0 32)

    if [ "$control" != "$prev_control" ]; then
        printf "[%s] CONTROL=%s (%s)\n" "$(date +%H:%M:%S)" "$control" "$(decode_control $control)"
        prev_control="$control"
    fi
    if [ "$back" != "$prev_back" ]; then
        printf "[%s] BACK_COLOR=%s\n" "$(date +%H:%M:%S)" "$back"
        prev_back="$back"
    fi
    if [ "$osd1" != "$prev_osd1" ]; then
        printf "[%s] OSD1_ADDR=%s\n" "$(date +%H:%M:%S)" "$osd1"
        prev_osd1="$osd1"
    fi
    if [ "$osd2" != "$prev_osd2" ]; then
        printf "[%s] OSD2_ADDR=%s\n" "$(date +%H:%M:%S)" "$osd2"
        prev_osd2="$osd2"
    fi
    if [ "$osd3" != "$prev_osd3" ]; then
        printf "[%s] OSD3_ADDR=%s\n" "$(date +%H:%M:%S)" "$osd3"
        prev_osd3="$osd3"
    fi
    if [ "$vid1" != "$prev_vid1" ]; then
        printf "[%s] VIDEO_ADDR1=%s\n" "$(date +%H:%M:%S)" "$vid1"
        prev_vid1="$vid1"
    fi
    if [ "$vid2" != "$prev_vid2" ]; then
        printf "[%s] VIDEO2_ADDR1=%s\n" "$(date +%H:%M:%S)" "$vid2"
        prev_vid2="$vid2"
    fi
    if [ "$prio" != "$prev_prio" ]; then
        printf "[%s] MODE_LCD_REG0(priority/blend_mode)=%s\n" "$(date +%H:%M:%S)" "$prio"
        prev_prio="$prio"
    fi
done

echo "=== done, ${DURATION}s elapsed ==="
