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
# --no-hardware: the black-screen leading theory is a GAL-pool vs fb0-
# offset mismatch (libdirectfb_gal.so allocates surfaces from galcore's
# own GPU memory via gcoSURF_Construct, separate from /dev/fb0's DTS-
# reserved buffer; primaryFlipRegion()'s yoffset math is only valid if
# the surface is fb0-backed -- if it's GAL-pool-backed instead, the pan
# ioctl succeeds but pans to a valid-but-wrong, never-written address).
# start_msn_directfb already sets QWS_DISPLAY's "systemonly" flag to
# force fb0-backed surface allocation (added 2026-07-22) -- if the
# black screen still happens with that in place, systemonly alone isn't
# sufficient (either it doesn't cover every surface DirectFB allocates,
# or this isn't the real cause). --no-hardware goes one step further:
# it forces DirectFB into pure software rendering, no GPU/GAL surfaces
# at all, via /etc/directfbrc's commented-out "no-hardware" line. If
# the black screen disappears (just slower) with --no-hardware but NOT
# with systemonly alone, that confirms GPU/GAL involvement as the root
# cause and narrows it to something systemonly doesn't cover. If it
# STILL happens even with --no-hardware, the GPU/GAL pool theory is
# ruled out entirely and the bug is elsewhere (colorkey, alpha
# compositing, or something outside DirectFB's rendering path).
#
# This flag backs up /etc/directfbrc, uncomments "no-hardware", runs
# the normal watch+start_msn_directfb flow, then restores the original
# file unconditionally (trap on EXIT/INT/TERM) -- no-hardware is
# explicitly not meant to ship enabled, so this never leaves it on.
#
# --no-systemonly: the complementary test. Rather than bypassing the
# GPU entirely, this keeps GPU acceleration on but REMOVES the
# "systemonly" QWS_DISPLAY flag, allowing DirectFB to fall through to
# its own default surface-pool selection (i.e. deliberately
# reintroducing the condition the 2026-07-22 systemonly fix was meant
# to prevent). Useful as a three-way comparison with the normal run:
#   - normal (systemonly on, hardware on) vs --no-systemonly (systemonly
#     off, hardware on): if these look IDENTICAL, systemonly isn't
#     actually changing pool selection in practice (the flag isn't
#     being respected, or GAL-pool allocation was never the
#     differentiator) -- look elsewhere. If --no-systemonly is WORSE
#     or different, systemonly is doing something real, just not
#     enough on its own.
#   - --no-systemonly vs --no-hardware: isolates whether it's pool
#     *selection* (systemonly) or GPU *usage* (hardware) that matters,
#     since --no-hardware makes pool selection moot (no GPU surfaces
#     exist at all under it).
# Implemented by exporting QWS_DISPLAY without "systemonly" before
# calling start_msn_directfb, which now respects an already-exported
# QWS_DISPLAY instead of always overwriting it (see start_msn_directfb
# itself). Combine with --no-hardware if useful, though note
# --no-hardware alone already makes systemonly irrelevant (no GPU
# surfaces to pool-select between).
#
# --stock-config: both "systemonly" (QWS_DISPLAY) and disabling
# no-layers-clear/no-surface-clear (directfbrc) turned out, on
# checking, to be OUR OWN theory-driven experimental changes from
# 2026-07-22 -- neither exists in stock's real, shipped config. Stock's
# actual etc/profile has QWS_DISPLAY=directfb:boundingrectflip:
# mmWidth220:mmHeight120:0 (no systemonly at all), and stock's actual
# etc/directfbrc has no-layers-clear/no-surface-clear ENABLED (we
# turned them off). This mode restores BOTH to stock's exact values --
# the closest static replication of stock's known-working config this
# reconstruction can produce. Hardware (GPU/GAL) stays ON, since stock
# uses it too. If the black screen survives --stock-config, that's
# strong evidence the bug lives inside the newer ported galcore
# 6.2.4.p1.8 / libGAL driver stack itself (a different driver
# generation than whatever stock's 3.4 kernel originally shipped with,
# on the same physical GPU) rather than in any env/config difference --
# matching the broader unresolved GPU/EffectWatch black-screen thread
# already tracked in this project's history.
#
# Backs up and restores /etc/directfbrc the same way --no-hardware
# does (trap on EXIT/INT/TERM), and exports QWS_DISPLAY the same way
# --no-systemonly does. Mutually exclusive with --no-hardware/
# --no-systemonly in practice (it sets both directfbrc lines and
# QWS_DISPLAY itself) -- don't combine.
#
# Usage:
#   lcd-osd1ctl-directfb-watch.sh [--no-hardware | --no-systemonly | --stock-config] [max seconds, default 120]
# Ctrl-C start_msn_directfb whenever you're done observing (black
# screen or not) -- the script cleans up and prints the full log
# either way. Also saved at /tmp/lcd-osd1ctl-directfb-watch.log.

LCDC=0xe0500000
CONTROL=$((LCDC + 0x04))
OSD1_CTL=$((LCDC + 0x74))
OSD1_ADDR=$((LCDC + 0x80))

MODE="normal"
case "$1" in
    --no-hardware|--no-systemonly|--stock-config)
        MODE="${1#--}"
        shift
        ;;
esac
DURATION="${1:-120}"
LOG=/tmp/lcd-osd1ctl-directfb-watch.log
DIRECTFBRC=/etc/directfbrc
DIRECTFBRC_BACKUP=/tmp/directfbrc.lcd-osd1ctl-watch.bak

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

restore_directfbrc() {
    if [ -f "$DIRECTFBRC_BACKUP" ]; then
        cp "$DIRECTFBRC_BACKUP" "$DIRECTFBRC"
        rm -f "$DIRECTFBRC_BACKUP"
        echo "[$MODE] restored original $DIRECTFBRC" >> "$LOG"
    fi
}

: > "$LOG"
echo "=== lcd-osd1ctl-directfb-watch.sh (mode: $MODE) ===" | tee -a "$LOG"

case "$MODE" in
    no-hardware)
        echo "Forcing pure software DirectFB rendering (no GPU/GAL surfaces at all)." | tee -a "$LOG"
        cp "$DIRECTFBRC" "$DIRECTFBRC_BACKUP"
        trap restore_directfbrc EXIT INT TERM
        sed -i 's/^#no-hardware$/no-hardware/' "$DIRECTFBRC"
        if ! grep -q '^no-hardware$' "$DIRECTFBRC"; then
            echo "WARNING: couldn't find the commented '#no-hardware' line in $DIRECTFBRC to enable -- check it hasn't moved/changed." | tee -a "$LOG"
        fi
        ;;
    no-systemonly)
        echo "Removing the 'systemonly' QWS_DISPLAY flag (GPU/GAL still on, pool selection unconstrained)." | tee -a "$LOG"
        export QWS_DISPLAY="directfb:boundingrectflip:mmWidth220:mmHeight120:0"
        ;;
    stock-config)
        echo "Restoring stock's exact real config: QWS_DISPLAY without 'systemonly', and directfbrc's no-layers-clear/no-surface-clear re-enabled (both of those are our own theory-driven divergences from stock, not things stock itself needs)." | tee -a "$LOG"
        export QWS_DISPLAY="directfb:boundingrectflip:mmWidth220:mmHeight120:0"
        cp "$DIRECTFBRC" "$DIRECTFBRC_BACKUP"
        trap restore_directfbrc EXIT INT TERM
        sed -i 's/^#no-layers-clear$/no-layers-clear/' "$DIRECTFBRC"
        sed -i 's/^#no-surface-clear$/no-surface-clear/' "$DIRECTFBRC"
        if ! grep -q '^no-layers-clear$' "$DIRECTFBRC" || ! grep -q '^no-surface-clear$' "$DIRECTFBRC"; then
            echo "WARNING: couldn't find the commented '#no-layers-clear'/'#no-surface-clear' lines in $DIRECTFBRC to enable -- check they haven't moved/changed." | tee -a "$LOG"
        fi
        ;;
    normal)
        echo "GPU/GAL on, systemonly on (this reconstruction's current defaults)." | tee -a "$LOG"
        ;;
esac

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
