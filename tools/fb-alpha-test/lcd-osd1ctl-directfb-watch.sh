#!/bin/sh
# lcd-osd1ctl-directfb-watch.sh -- correlates OSD1_CTL (rgb_order/yuv_order/
# format) and OSD1 layer-enable/frame-address state against a DirectFB
# session, to chase two open bugs at once: colors still swapped, and
# DirectFB still going black. Backgrounds a register poller, launches
# start_msn_directfb in the foreground, and dumps everything once you
# Ctrl-C out of it (whether or not the black screen happened).
#
# What we're checking:
#   1. rgb_order persistence -- our kernel driver sets OSD1_CTL's
#      rgb_order field (bits 18-20) to 5 at fb_set_par() time, derived
#      from pdata->lcd_wiring_mode (itself from arkdata.ini's
#      RgbMode=5 via the DTB lcd-wiring-mode="RGB" property). U-Boot's
#      own bootlogo render confirms that value (5) produces correct
#      colors at the register level. If the kernel's later render still
#      shows swapped colors, either (a) something overwrites rgb_order
#      after fb_set_par() runs -- candidates: ark168vin_set_display()'s
#      VIN_SET_WINDOW_FORMAT case and ark1668_lcdc_display_update_atomic()'s
#      ATOMIC_SET_LAYER_FMT case in ark1668_lcdc_funcs.c, both of which
#      take rgb_order straight from a userspace ioctl argument,
#      independent of pdata->lcd_wiring_mode -- or (b) rgb_order was
#      never the real bug for this symptom in the first place. This
#      script tells us which: if OSD1_CTL's rgb_order field changes
#      value at any point after baseline, (a) is confirmed.
#   2. The black-screen bug -- does CONTROL's OSD1-enable bit or
#      OSD1_ADDR do anything unexpected (disable, jump to an
#      unexpected address) around when the screen goes black? Same
#      registers lcd-overlay-watch.sh already covers for the red-
#      overlay bug, reused here for correlation with rgb_order in one
#      combined timeline instead of two separate runs.
#
# NOTE on stock's real rgb_order encoding, found via a debug string in
# stock's own kernel binary: "0=rgb, 1=rbg, 2=grb, 3=gbr, 4=brg,
# 5=bgr" -- NOT the same order as this reconstruction's own
# ARK_LCDC_WIRING_* enum (0=BGR...5=RGB). The round trip (arkdata.ini
# RgbMode=5 -> DTB "RGB" string -> kernel enum ARK_LCDC_WIRING_RGB=5 ->
# register value 5) is self-consistent by construction, so the naming
# mismatch alone shouldn't be the bug -- but keep it in mind reading
# the numeric register values below; "rgb_order=5" in this log means
# stock's own "bgr" encoding, which is the value U-Boot's bootlogo
# already confirmed correct for this panel's real wiring.
#
# Also see tools/fb-alpha-test/lcd-blend-sweep.sh's own notes: an
# earlier session already exhaustively swept rgb_order 0-7 for a
# *different* symptom (partial-alpha-blend color corruption) and found
# it insufficient alone -- but that same sweep found fully-opaque
# pixels always rendered correctly regardless of rgb_order. If this
# run shows rgb_order holding steady at 5 the whole time and colors
# are STILL wrong on solid (opaque) UI content, that's new evidence
# rgb_order isn't the culprit for this particular symptom either, and
# the next step should look elsewhere (DirectFB surface pixel format,
# Y2R coefficients, or GAL/libGAL.so's own color handling).
#
# Usage:
#   lcd-osd1ctl-directfb-watch.sh [max seconds to run, default 120]
# Ctrl-C start_msn_directfb whenever you're done observing (black
# screen or not) -- the script cleans up and prints the full log
# either way. Also saved at /tmp/lcd-osd1ctl-directfb-watch.log.

LCDC=0xe0500000
CONTROL=$((LCDC + 0x04))
OSD1_CTL=$((LCDC + 0x74))
OSD1_ADDR=$((LCDC + 0x80))

DURATION="${1:-120}"
LOG=/tmp/lcd-osd1ctl-directfb-watch.log

decode_osd1ctl() {
    val="$1"
    fmt=$(( (val >> 12) & 0xF ))
    rgb=$(( (val >> 18) & 0x7 ))
    yuv=$(( (val >> 21) & 0x3 ))
    printf "format=%d rgb_order=%d(stock-encoding) yuv_order=%d" "$fmt" "$rgb" "$yuv"
}

decode_control() {
    val="$1"
    o1=$(( (val >> 7) & 1 ))
    printf "OSD1_enabled=%d" "$o1"
}

: > "$LOG"
echo "=== lcd-osd1ctl-directfb-watch.sh ===" | tee -a "$LOG"
echo "Baseline (before starting DirectFB):" | tee -a "$LOG"
base_ctl=$(devmem $OSD1_CTL 32)
base_control=$(devmem $CONTROL 32)
base_addr=$(devmem $OSD1_ADDR 32)
printf "  OSD1_CTL=%s (%s)\n" "$base_ctl" "$(decode_osd1ctl "$base_ctl")" | tee -a "$LOG"
printf "  CONTROL=%s (%s)\n" "$base_control" "$(decode_control "$base_control")" | tee -a "$LOG"
printf "  OSD1_ADDR=%s\n" "$base_addr" | tee -a "$LOG"
echo | tee -a "$LOG"

(
    prev_ctl=""
    prev_control=""
    prev_addr=""
    end=$(( $(date +%s) + DURATION ))
    while [ "$(date +%s)" -lt "$end" ]; do
        ctl=$(devmem $OSD1_CTL 32)
        control=$(devmem $CONTROL 32)
        addr=$(devmem $OSD1_ADDR 32)

        if [ "$ctl" != "$prev_ctl" ]; then
            printf "[%s] OSD1_CTL=%s (%s)\n" "$(date +%H:%M:%S)" "$ctl" "$(decode_osd1ctl "$ctl")" >> "$LOG"
            prev_ctl="$ctl"
        fi
        if [ "$control" != "$prev_control" ]; then
            printf "[%s] CONTROL=%s (%s)\n" "$(date +%H:%M:%S)" "$control" "$(decode_control "$control")" >> "$LOG"
            prev_control="$control"
        fi
        if [ "$addr" != "$prev_addr" ]; then
            printf "[%s] OSD1_ADDR=%s\n" "$(date +%H:%M:%S)" "$addr" >> "$LOG"
            prev_addr="$addr"
        fi
    done
) &
POLLER_PID=$!

killall MsnCoreApp 2>/dev/null
echo "Starting start_msn_directfb -- watch the screen, Ctrl-C when done (black screen or not)..." | tee -a "$LOG"
start_msn_directfb

kill "$POLLER_PID" 2>/dev/null
wait "$POLLER_PID" 2>/dev/null

echo | tee -a "$LOG"
echo "=== Full log ($LOG) ==="
cat "$LOG"
